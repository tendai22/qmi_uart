# RP2350 QMI Flash / Protothread 設計メモ 最終版

## 第I部 実装編

### 第1章 背景
現在のシステムは RP2350 + TinyUSB CDC + QMI Flash を RTOS 非使用、cooperative scheduling で実装している。

```cpp
for(;;)
{
    tud_task();
    qmi_flash.poll();
    cache_manager.poll();
}
```

目標は tud_task() を高頻度で回しながら、フラッシュ処理を逐次処理風に記述すること。

### 第2章 Future ではなく Coroutine
今回欲しいのは結果格納箱(Future)ではなく、

- 処理中断(yield)
- 再開(resume)

である。

```text
Future     = 非同期結果
Coroutine  = 中断と再開
```

### 第3章 状態機械実装の課題

```cpp
switch(state)
{
case ST_ERASE_SEND_WREN:
case ST_ERASE_WAIT_WREN:
case ST_ERASE_SEND_CMD:
case ST_ERASE_WAIT_BUSY:
}
```

状態数増加と見通し悪化が問題。

### 第4章 Protothread 採用理由
保存対象は再開位置と最小限の状態のみ。

```cpp
struct Context
{
    uint16_t pt_line;
    uint32_t addr;
    uint32_t offset;
    uint8_t* buffer;
};
```

### 第5章 BSD sleep/wakeup との類似

```c
start_io();
sleep(chan);
wakeup(chan);
```

↓

```cpp
start_io();
WAIT_UNTIL(done());
```

### 第6章 Continuation との関係
Protothread は「必要最小限の continuation を保持する仕組み」と捉える。

### 第7章 Protothread 動作原理

```cpp
#define PT_WAIT_UNTIL(cond)  do {    pt_line = __LINE__;    return false;    case __LINE__:;    if(!(cond)) return false;  } while(0)
```

### 第8章 ループ途中から再開できる理由

```cpp
for(offset=0; offset<256; offset+=8)
{
    send8();
    PT_WAIT_UNTIL(tx_done());
}
```

offset を Context として保持する。

### 第9章 QMI Flash への適用対象

- read()
- erase_4k()
- program_page()

### 第10章 得られている知見

- Read: 8バイト単位受信で十分
- Program: 8バイト単位送信で十分
- Erase: Busy解除待ち主体

### 第11章 erase_4k 理想形

```cpp
PT_BEGIN();

write_enable();
WAIT_FLASH();

send_erase(addr);
WAIT_FLASH();

while(busy())
{
    read_status();
    WAIT_FLASH();
}

PT_END();
```

### 第12章 page_program 理想形

```cpp
PT_BEGIN();

write_enable();
WAIT_FLASH();

send_program_command();
WAIT_FLASH();

for(offset=0; offset<256; offset+=8)
{
    send8(src+offset);
    WAIT_FLASH();
}

WAIT_BUSY_CLEAR();
PT_END();
```

### 第13章 CacheManager への波及

```cpp
load_page();
WAIT_FLASH();

modify();

flush_page();
WAIT_FLASH();
```

### 第14章 実装順序
1. Protothread基盤
2. Status read
3. erase_4k
4. program_page
5. read
6. CacheManager

### 第15章 状態機械→Protothread対応表

#### Erase

| State | Protothread |
|---|---|
| ST_ERASE_SEND_WREN | write_enable(); |
| ST_ERASE_WAIT_WREN | WAIT_FLASH(); |
| ST_ERASE_SEND_CMD | send_erase(); |
| ST_ERASE_WAIT_BUSY | while(busy()){...} |

#### Program

| State | Protothread |
|---|---|
| ST_PROGRAM_SEND_DATA | send8(); |
| ST_PROGRAM_WAIT_TX | WAIT_FLASH(); |
| ST_PROGRAM_WAIT_BUSY | WAIT_BUSY_CLEAR(); |

## 第II部 設計・運用編

### 第16章 Protothread実装上の注意

- ローカル変数は保存されない
- switchネストを避ける
- WAITマクロを通常関数内で使わない
- wait_reason を持つ

```cpp
enum WaitReason
{
    WAIT_TX,
    WAIT_RX,
    WAIT_STATUS,
    WAIT_BUSY_CLEAR
};
```

### 第17章 QMI Flash 実装計画

#### ReadStatusTask

```cpp
send_rdsr();
WAIT_TX();
read_sr1();
WAIT_RX();
```

#### EraseTask

```cpp
write_enable();
WAIT_FLASH();
send_erase4k();
WAIT_FLASH();
```

#### ProgramTask

```cpp
for(offset=0; offset<256; offset+=8)
{
    send8();
    WAIT_TX();
}
```

#### 共通Context

```cpp
struct QmiTaskContext
{
    uint16_t pt_line;
    uint32_t flash_addr;
    uint32_t offset;
    uint32_t size;
    uint8_t* buffer;
    uint8_t sr1;
    WaitReason wait_reason;
};
```

### 第18章 CacheManager展開構想

状態中心

```text
STATE_LOAD
STATE_WAIT_LOAD
STATE_WAIT_PROGRAM
```

から

```cpp
load_page();
WAIT_FLASH();
modify();
flush_page();
WAIT_FLASH();
```

へ移行する。

## 第III部 思想編

### 第19章 Protothread と Continuation

#### なぜ Continuation を連想したか
return しているのに継続実行できるため。

#### 完全 Continuation との違い

完全 Continuation:
- スタック
- ローカル変数
- 呼出し履歴

を保存。

Protothread:

```cpp
struct Context
{
    uint16_t pt_line;
    uint32_t offset;
};
```

程度のみ保存。

#### 何を捨てているか
スタック保存を捨てる。

#### なぜ再開できるか

```text
pt_line
+
offset
```

が保存されているから。

#### BSD との対応

```c
sleep();
wakeup();
```

↓

```cpp
WAIT_UNTIL(done());
```

#### 本設計での結論

Protothread は

```text
switch-case の黒魔術
```

ではなく

```text
Continuation の保存対象を
組込み向けに極限まで削ったもの
```

として理解する。

そして QMI Flash、CacheManager を

```cpp
load_page();
WAIT_FLASH();
modify();
flush_page();
WAIT_FLASH();
```

のようなアルゴリズム中心の記述へ移行する。
