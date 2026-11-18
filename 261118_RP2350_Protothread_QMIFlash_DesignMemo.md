# RP2350 QMI Flash と Protothread 設計メモ

作成日: 2026-11-18

## 背景

当初は pico-sdk の C/C++ 環境で `std::future` / `std::async` を利用して、
TinyUSB CDC とフラッシュアクセスの上位層を非同期記述できないか検討した。

しかし RP2040/RP2350 の bare metal 環境では、一般的な `future` は
スレッドや OS ランタイムを前提としており、今回の用途には適さないと判断した。

## 方針転換

目標は future ではなく、

- switch + state machine 地獄の回避
- シーケンシャルな記述
- cooperative scheduling 維持
- TinyUSB の `tud_task()` を定期実行

である。

そのため、RTOS や future ではなく、Protothread 的な stackless coroutine を採用する方向とした。

---

## 現在の実行モデル

下位層では既に以下をポーリングベースで実現済み。

- TinyUSB CDC
- 内蔵フラッシュアクセス
- 単一コア
- cooperative scheduling

メインループは概ね以下の形。

```cpp
for (;;)
{
    tud_task();

    flash.poll();

    cache.poll();
}
```

---

## Protothread の考え方

Protothread は本格的な coroutine ではない。

保存するのは主に

- 再開位置(line番号)
- 必要な状態変数

のみ。

概念的には

```cpp
struct Context {
    int line;

    uint32_t addr;
    uint32_t offset;
    uint8_t status;
};
```

のような continuation の極小版として理解できる。

### Continuation との比較

本格的 continuation

- スタック全体を保存可能
- 強力だが重い

Protothread

- 再開位置のみ保存
- 必要なローカル変数は自分で保持
- 非常に軽量

---

## Protothread の典型例

```cpp
flash.read_start(addr, buf);

WAIT_UNTIL(!flash.busy());

modify();

flash.erase_start(addr);

WAIT_UNTIL(!flash.busy());

flash.program_start(addr, buf);

WAIT_UNTIL(!flash.busy());
```

これは昔の BSD カーネルで見られた

```c
start_io();

sleep(chan);

wakeup(chan);
```

に近い感覚で理解できる。

---

## ループ内で yield できる理由

例えば

```cpp
for(offset = 0;
    offset < 256;
    offset += 8)
{
    qmi_write8(...);

    PT_WAIT_UNTIL(qmi_tx_complete());
}
```

が成立する。

重要な点は、

```cpp
offset
```

をローカル変数ではなくタスクオブジェクトのメンバとして持つこと。

```cpp
class ProgramTask
{
public:
    uint32_t offset;
};
```

return 後に再実行されても offset が保持されているため、
次回 poll 時に継続実行できる。

---

## QMI Flash への適用

まず CacheManager より先に、
既存の QMI Flash ドライバを Protothread 化する。

理由:

- 同期版 read/erase/program が既に存在
- アルゴリズムが明確
- TinyUSB 制約との共存を確認しやすい

### 実装対象

- read
- erase_4k
- program_page

---

## 既に分かっている知見

### Read

- 小分けに受信すればよい
- 8バイト単位程度で十分

### Erase

- busy ビット解除待ちのみ

### Program

- 8バイト単位送信
- SR1 Busy 完了待ち

いずれも cooperative scheduling と相性が良い。

---

## erase_4k の理想形

状態機械ではなく、概念的に以下のように記述したい。

```cpp
PT_BEGIN();

write_enable();
WAIT_FLASH();

send_erase(addr);
WAIT_FLASH();

while (busy())
{
    read_status();
    WAIT_FLASH();
}

PT_END();
```

---

## page program の理想形

```cpp
PT_BEGIN();

write_enable();
WAIT_FLASH();

send_program_command();
WAIT_FLASH();

for(offset = 0;
    offset < 256;
    offset += 8)
{
    send8(...);

    WAIT_FLASH();
}

WAIT_BUSY_CLEAR();

PT_END();
```

状態遷移名ではなく、実際の処理手順がそのまま見える。

---

## 今後の実装方針

1. Protothread 基盤を自作する
2. status read を Protothread 化
3. erase_4k を Protothread 化
4. program_page を Protothread 化
5. read を Protothread 化
6. 有効性確認後に CacheManager へ展開

---

## 設計上の理解

今回の Protothread は

「switch-case の黒魔術」

としてではなく、

「必要最小限の continuation を自分で保持する仕組み」

として捉える。

保持対象は

- 再開位置
- アドレス
- オフセット
- 状態レジスタ
- バッファポインタ

程度。

その軽い意志を持って設計を進める。
