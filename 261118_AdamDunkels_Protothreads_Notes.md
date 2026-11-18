# Adam Dunkels Protothreads 調査メモ

## 概要

Protothreads は、Adam Dunkels が組込みシステム向けに考案した軽量な stackless coroutine である。

特徴:

- スレッドごとのスタックを持たない
- C言語のみで実装可能
- switch/case とマクロで継続実行を実現
- 協調型スケジューリング(cooperative scheduling)
- RAM 消費が極めて小さい

代表的な適用先:

- Contiki OS
- センサネットワーク機器
- 小規模マイコン
- 通信プロトコルスタック

---

# Adam Dunkels について

Adam Dunkels はスウェーデンのコンピュータ研究者で、
軽量 TCP/IP スタック uIP や Contiki OS の作者として知られる。

Protothreads は

「イベント駆動(state machine)より読みやすく、RTOSスレッドより軽い」

という中間解を目指して考案された。

---

# Protothreads の定義

Protothread とは

> スタックを持たず、再開位置のみを保存する軽量 coroutine

である。

保存対象:

```cpp
struct Context
{
    uint16_t line;
};
```

発展形:

```cpp
struct Context
{
    uint16_t line;

    uint32_t addr;
    uint32_t offset;

    uint8_t* buffer;
};
```

---

# Continuation との関係

完全 Continuation:

- スタック保存
- ローカル変数保存
- 呼出し履歴保存

Protothread:

- 再開位置のみ保存
- 必要な変数は自分で保持

つまり

```text
Continuation の保存対象を
極限まで削ったもの
```

と理解できる。

---

# 基本マクロ定義例

## コンテキスト

```c
struct pt
{
    unsigned short lc;
};
```

## PT_BEGIN

```c
#define PT_BEGIN(pt)     switch((pt)->lc) {     case 0:
```

## PT_END

```c
#define PT_END(pt)     }     (pt)->lc = 0;     return 1;
```

## PT_WAIT_UNTIL

```c
#define PT_WAIT_UNTIL(pt, condition)     do {         (pt)->lc = __LINE__;         case __LINE__:         if(!(condition)) return 0;     } while(0)
```

戻り値:

```text
0 = 継続中
1 = 完了
```

---

# 動作イメージ

```cpp
PT_BEGIN(&pt);

send_command();

PT_WAIT_UNTIL(&pt, tx_complete());

read_response();

PT_WAIT_UNTIL(&pt, rx_complete());

PT_END(&pt);
```

内部的には

```cpp
line = 123;
return;

case 123:
```

に変換される。

---

# サンプル1: CacheManager

目的:

- キャッシュミス時ロード
- dirty 管理
- flush

```cpp
bool CacheManager::poll()
{
    PT_BEGIN(&pt);

    while (true)
    {
        PT_WAIT_UNTIL(&pt, has_request());

        page = lookup(lba);

        if(page == nullptr)
        {
            flash_read_start(lba);

            PT_WAIT_UNTIL(
                &pt,
                flash_complete());

            page = allocate_page();
        }

        modify(page);

        page->dirty = true;

        if(page->dirty)
        {
            flash_flush_start(page);

            PT_WAIT_UNTIL(
                &pt,
                flash_complete());

            page->dirty = false;
        }
    }

    PT_END(&pt);
}
```

---

# サンプル2: qmi_flash_erase4k

状態機械版では

```text
WREN
WAIT_WREN
ERASE
WAIT_BUSY
DONE
```

になる処理。

Protothread版:

```cpp
bool EraseTask::poll()
{
    PT_BEGIN(&pt);

    write_enable();

    PT_WAIT_UNTIL(
        &pt,
        tx_complete());

    send_erase4k(addr);

    PT_WAIT_UNTIL(
        &pt,
        tx_complete());

    while (true)
    {
        read_status_register();

        PT_WAIT_UNTIL(
            &pt,
            rx_complete());

        if(!(sr1 & SR_BUSY))
            break;
    }

    PT_END(&pt);
}
```

---

# サンプル3: qmi_flash_program_page

RP2350 実験方針:

- 8バイト単位送信
- tud_task() を定期的に回す

```cpp
bool ProgramTask::poll()
{
    PT_BEGIN(&pt);

    write_enable();

    PT_WAIT_UNTIL(
        &pt,
        tx_complete());

    send_program_command(addr);

    PT_WAIT_UNTIL(
        &pt,
        tx_complete());

    for(offset = 0;
        offset < 256;
        offset += 8)
    {
        send8(buffer + offset);

        PT_WAIT_UNTIL(
            &pt,
            tx_complete());
    }

    while(true)
    {
        read_status_register();

        PT_WAIT_UNTIL(
            &pt,
            rx_complete());

        if(!(sr1 & SR_BUSY))
            break;
    }

    PT_END(&pt);
}
```

---

# 設計上の重要ポイント

## ローカル変数は保存されない

NG:

```cpp
void poll()
{
    uint32_t offset;

    PT_WAIT_UNTIL(...);
}
```

OK:

```cpp
class ProgramTask
{
    uint32_t offset;
};
```

## 保存するのは continuation の一部

```cpp
struct ProgramContext
{
    uint16_t pt_line;

    uint32_t flash_addr;
    uint32_t offset;

    uint8_t* buffer;

    uint8_t sr1;
};
```

## 本資料での結論

Protothread は単なる switch/case テクニックではない。

```text
必要最小限の continuation を
設計者自身が管理する仕組み
```

として理解すると、QMI Flash や CacheManager のような協調型非同期処理をアルゴリズム中心に記述できる。

## 参考文献

### Protothreads 公式

Adam Dunkels 自身による Protothreads の公式ページ。

Adam Dunkels, Protothreads: Lightweight, Stackless Threads in C
https://dunkels.com/adam/pt/

特に以下は必読。

Under the Hood
API Reference
Example Programs

Protothread が

```
switch
case
__LINE__
```

### 論文 Protothreads: Simplifying Event-Driven Programming of Memory-Constrained Embedded Systems

Adam Dunkels

国際会議 SenSys 2006 に掲載。

正式引用:

```
Adam Dunkels, Oliver Schmidt, Thiemo Voigt, and
Muneeb Ali.

Protothreads: Simplifying Event-Driven Programming
of Memory-Constrained Embedded Systems.

Proceedings of the 4th International Conference
on Embedded Networked Sensor Systems (SenSys 2006).
```

この論文は Protothread を学ぶ上で最も重要な一次資料である。

### Contiki OS

Protothread の実運用例。

Contiki Operating System
Contiki-NG

公式: `https://contiki-ng.org/`

Protothread が TCP/IP スタックやネットワークプロトコル実装に組み込まれている。

### Coroutine の背景

#### The Art of Computer Programming

Donald Knuth
The Art of Computer Programming
Volume 1

Coroutine の古典的解説がある。

#### Conway (1963)

Coroutine という名称の起源。

Melvin Conway

Design of a Separable Transition-Diagram Compiler

Communications of the ACM
1963

#### Continuation

Scheme

```
Harold Abelson
Gerald Jay Sussman

Structure and Interpretation
of Computer Programs
```

call/cc による Continuation の考え方を学ぶ代表文献。

熊谷さんが今回 Protothread を

continuation の極小版

として理解する際の理論的背景になる。

#### Duff's Device

Protothread 実装の理解に役立つ。Tom Duff による有名な C テクニック。Protothread の

```
switch (...)
{
case ...
```

Tom Duff, Duff's Device, 1983

### License

#### Protothreads ライブラリ

Adam Dunkels の Protothreads 実装は BSD スタイルライセンスで公開されている。

#### この設計メモとの関係

```
PT_BEGIN
PT_END
PT_WAIT_UNTIL
```

などは Protothreads のアイデアおよび公開マクロ定義を参考にしている。

ただし本資料中の

CacheManager 例
RP2350 QMI Flash 例
EraseTask
ProgramTask
Continuation 解説

は本プロジェクト向けの説明・派生設計例であり、Protothreads 本体のソースコードそのものではない。

### 後年の自分へのメモ

本資料の根底にある考え方は、

```
Future
↓
Coroutine
↓
Protothread
↓
Continuation の縮退版
```

特に Protothread を「switch/case の黒魔術」でなく、
「必要最小限の continuation を設計者自身が保持する仕組み」
として理解すると、CacheManager やフラッシュドライバの状態機械をアルゴリズム中心のコードへ整理しやすくなる。

