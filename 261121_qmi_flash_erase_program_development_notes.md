# RP2350 QMI Flash — Erase / Program の設計とデバッグ記録

> 年末同人誌向け・開発記録原稿（2026年11月21日時点）  
> 対象：Raspberry Pi Pico 2（RP2350）／Winbond W25Q32RV／Pico SDK／QMI Direct Mode  
> 記述範囲：本プロジェクトの対話・実験ログで確認できた内容。未完了の異常系対策は明記した。

## はじめに：何を作っているか

本プロジェクトでは、RP2350から外付けSPI NOR Flash（W25Q32RV）を操作し、レトロコンピュータ向けの仮想フロッピーを構成する。Flash操作の基礎となる4 KiB Sector Eraseと256バイト Page Programを、`start/poll`型の非同期処理として実装している。

初期には `qmi_flash_async_*` による実装も動作していたが、今回の作業では独自の `protothread.h` を利用し、1回の `poll`で行う仕事を小さく区切りながら処理を進める構造を試作した。QMI Direct ModeでSPI命令を送る部分は同期的な短い処理を残し、Flash内部のBUSY待ちやPage Programのデータ送信を複数回のpollに分割する。

### 開発条件

| 項目 | 内容 |
|---|---|
| MCU | Raspberry Pi RP2350（Pico 2） |
| Flash | Winbond W25Q32RV、4 MiB |
| Flash消去単位 | 4 KiB |
| Flash書き込み単位 | 256バイト／ページ |
| アクセス | QMI Direct Mode、Single SPI |
| システムクロック | 150 MHz（開発時に確認） |
| QMI SCK | 約5 MHz（分周値30のテスト設定） |
| 開発対象 | `erase_4k`、`program_page` のProtothread化 |

Flashの対象アドレスの例として、テストログでは `0x00380000` から4 KiBを使用した。これはその時点のテスト対象であり、仮想フロッピーの最終的な配置仕様そのものを示すものではない。

---

## 第1章　設計の骨格：Protothreadと共有コンテキスト

### 1.1 `start/poll`の考え方

コマンド発行側は、共有コンテキスト `pt` にコマンドとパラメータを設定する。poll側は `pt->command` を見て、対応するProtothread本体だけを呼ぶ。処理中の継続位置は `pt->line` に保存し、終了時に `pt->done` を立てる。

代表的な操作識別子は次のとおり。

```c
// start/pollの操作識別子
#define CMD_ERASE   1
#define CMD_READ    2
#define CMD_PROGRAM 3

// W25Q32RVに実際に送信するSPI命令
#define CMD_READ_SR1        0x05
#define CMD_READ_DATA       0x03
#define CMD_WRITE_ENABLE    0x06
#define CMD_SECTOR_ERASE_4K 0x20
#define CMD_PAGE_PROGRAM    0x02
```

**教訓：** `CMD_PROGRAM`（ソフトウェア上の操作種別）と `CMD_PAGE_PROGRAM`（Flashに送る命令コード）は、値も用途も別物である。初期の `pt_issue_program()` ではこの取り違えが見つかり、整理した。

### 1.2 復帰位置を保存するマクロ

```c
#define PT_WAIT_UNTIL_L(pt, cond, label) \
    do { \
        case __LINE__: \
        pt->line = __LINE__; \
        if (!(cond)) \
            return; \
    } while (0)
```

`PT_BEGIN(pt)` が `switch (pt->line)` を開始する前提で、このマクロは次のように働く。

1. `case __LINE__:` に入る。
2. 現在位置を `pt->line` に保存する。
3. 条件が成立しなければ呼び出し元へ戻る。
4. 次回のpollで同じ `case` に復帰し、**条件を再評価する**。

#### ハマり①：`case` を条件判定の後に置いてしまった

初期のマクロは概念的に次の配置だった。

```c
pt->line = __LINE__;
if (!(cond))
    return;
case __LINE__:;
```

この形では、次回のpollが `case` へジャンプした際に条件評価を飛び越してしまう。結果として、待機すべき状態を待たずに次の処理へ進み得る。

**解決：** `case __LINE__:` を条件評価の前へ移す。これはProtothreadの正しさを左右する本質的な修正だった。

> 注意：`label` 引数はマクロ本文には現れない。プロジェクトでは状態名の生成・ログ表示用に使う設計であり、行番号による復帰機構とは別の役割を持つ。

### 1.3 共通pollラッパー

EraseとProgramの本体を関数ポインタで受け、実行時間と状態遷移を共通に記録した。

```c
typedef void (*pt_body_func_t)(void);

static void pt_qmi_flash_poll_body_wrapper(pt_body_func_t body_func)
{
    int old_line = pt->line;
    uint32_t poll_start = time_us_32();

    body_func();

    uint32_t now = time_us_32();
    uint32_t poll_elapsed = now - poll_start;

    // old_line と pt->line の変化を記録し、
    // pt_stats_enter()/pt_stats_leave() に実行時間を渡す。
    // （ここではログ整形部分を省略）
}
```

**設計上の注意：** このラッパーは共有 `pt` を使う。各コマンドの本体を無条件に全部呼ぶ構造にしてはいけない。後述の大きなデバッグポイントになった。

---

## 第2章　Sector Erase：BUSY待ちとDirect Modeの落とし穴

### 2.1 基本シーケンス

4 KiB消去の基本操作は次のとおり。

1. パラメータチェック。
2. QMI Direct Modeを開始。
3. 前回のFlash操作がREADYになるまで待つ。
4. WREN（Write Enable、`0x06`）を送る。
5. CSをLowにしてSector Erase（`0x20`）と24-bitアドレスを送る。
6. 送信完了後、CSをHighにして消去を開始させる。
7. SR1のBUSYビットが解除されるまでpollする。
8. Direct Modeを終了し、コマンド完了を通知する。

```c
// Flashが前の操作を終えるまで待つ
PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(),
                WAIT_PREV_CMD_COMPLETE);
if (pt->error)
    goto error;

// WREN、Erase命令＋アドレス送信（中略）
qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS0N_BITS;

// Erase完了待ち
PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(),
                WAIT_ERASE_COMP);
if (pt->error)
    goto error;
```

### 2.2 ハマり②：SR1ヘルパーのDirect Mode二重開始・終了

SR1読み出しヘルパーを初めて非同期待ちに組み込んだ際、内部で `qmi_direct_mode_start()` と `qmi_direct_mode_stop()` を呼んでいた。しかし、Erase本体はすでにDirect Modeを開始済みだった。

```c
// 問題のあった設計（要旨）
bool pt_erase_sr1_ready(void)
{
    bool ready;
    if (!qmi_direct_mode_start()) { /* ... */ }
    bool read_ok = qmi_flash_status_ready(&ready);
    bool stop_ok = qmi_direct_mode_stop();
    /* ... */
    return ready;
}
```

結果としてQMI操作が破綻し、`tx_ready timeout` が発生した。ログには約622 msのタイムアウトが現れた。

**原因：** Direct Modeの開始・終了責任が、外側のコマンド本体と内側のSR1ヘルパーに二重化していた。

**解決：** Direct Modeの所有権をErase本体へ一本化。SR1ヘルパーは「Direct Modeが有効な状態でSR1を読む」ことだけに専念する。

```c
bool pt_erase_sr1_ready(void)
{
    bool ready;
    if (!qmi_flash_status_ready(&ready)) {
        pt->error = 4;
        return true;
    }
    return ready;
}
```

この変更後、Eraseは正常動作した。

### 2.3 ハマり③：`WAIT_START_ERASE` は本当に必要か

当初は次の2段階待ちを設けた。

```c
PT_WAIT_UNTIL_L(pt, !pt_erase_sr1_ready(), WAIT_START_ERASE);
PT_WAIT_UNTIL_L(pt,  pt_erase_sr1_ready(), WAIT_ERASE_COMP);
```

前者は「FlashがBUSYになったこと」を確認し、後者は「READYに戻ったこと」を待つもの。しかし、短いBUSY期間を見逃すと前者で待ち続ける危険がある。開発中の正常系では、CSをHighにした後にREADYを待つだけの構成へ簡略化した。

```c
PT_WAIT_UNTIL_L(pt, pt_erase_sr1_ready(), WAIT_ERASE_COMP);
if (pt->error)
    goto error;
```

この設計は正常系を簡潔にする一方、**Erase命令が実際に受理されたことまで検証するものではない**。異常系の扱いは今後の課題として残した。

また、読み出し失敗時に `true` を返すヘルパーは、肯定条件のREADY待ちを抜けるための便宜である。否定条件 `!pt_erase_sr1_ready()` に使うと、エラー時に待機から抜けられない可能性がある。ヘルパーのエラー伝播は将来的に再設計したい。

### 2.4 Erase実測とデバッグ出力の観測者効果

Eraseの状態遷移ログ例：

```text
START -> WAIT_ERASE_COMP: 30 us, count = 0
WAIT_ERASE_COMP -> START: 1232 us, count = 245
```

合計約1.26 ms。4 KiBの読み戻しでは全バイト `0xFF` を確認した。

初期のログではpollの開始間隔が約20～21 µs、処理本体の経過時間が約5 µsだった。原因は、毎回のpollで実行していた `debug()` の文字列整形などの負荷だった。状態遷移時のみログを残す方式へ変更すると、連続pollの間隔は約5～6 µsとなった。

**教訓：** マイクロ秒単位の測定では、デバッグ記録そのものがタイミングを変える。状態遷移時だけログを記録し、poll本体の実行時間と状態滞在時間を区別して扱う。

---

## 第3章　Page Program：8バイトずつ送信する

### 3.1 パラメータチェック

今回のPage Programは、ページ先頭アドレスから最大256バイトを書き込む仕様とした。

```c
bool pt_program_page_param_check(void)
{
    if (pt->buffer == NULL)
        return false;
    if (pt->length == 0 || pt->length > PAGE_SIZE)
        return false;
    if ((pt->address & (PAGE_SIZE - 1u)) != 0)
        return false;
    if (pt->address >= FLASH_SIZE)
        return false;
    if (pt->length > FLASH_SIZE - pt->address)
        return false;
    if ((pt->address % PAGE_SIZE) + pt->length > PAGE_SIZE)
        return false;
    return true;
}
```

`FLASH_SIZE - address` で範囲判定するのは、`address + length` の整数オーバーフローを避けるため。現在はアドレスのページアラインメントと `length <= PAGE_SIZE` を要求するので、最後の境界チェックは論理的には冗長だが、仕様の明示として残している。

### 3.2 8バイトずつ送る理由

1ページ256バイトを一度に送らず、pollごとに最大8バイトを送る。CSをLowに維持している間は、同じPage ProgramのSPIトランザクションが続く。

```c
static bool pt_write_chunk_bytes(void)
{
    int n = pt->length - pt->count;
    if (n > 8)
        n = 8;

    for (int i = 0; i < n; ++i) {
        if (!qmi_flash_write_byte(pt->buffer[pt->count])) {
            pt->error = 5;
            return true;  // 待機を抜けてエラー処理へ
        }
        pt->count++;
    }
    return pt->count >= pt->length;
}
```

呼び出し側：

```c
PT_WAIT_UNTIL_L(pt, pt_write_chunk_bytes(), WRITE_BYTES);
if (pt->error)
    goto error;
```

**注意：** 送信失敗時に `false` を返すだけでは、Protothreadが再度送信を試みてしまう。エラーコードを設定して待機を抜け、直後に `pt->error` を検査する。また、`pt->count++` は送信成功後に行う。

### 3.3 Page Programの流れ

```text
パラメータ確認
  ↓
Direct Mode開始
  ↓
前回操作のREADY待ち
  ↓
WREN
  ↓
CS Low → 0x02 + 24-bit address
  ↓
8バイト送信 × 32回（256バイトの場合）
  ↓
QMI送信完了待ち → CS High
  ↓
SR1 READY待ち
  ↓
Direct Mode終了 → done=true
```

データ送信の後でCSをHighにすることが重要。途中でCSを上げると1ページ分のトランザクションが分断される。

---

## 第4章　ハマり④：最初の8バイトから進まない

### 4.1 最初はpollの呼び忘れ

最初の試験では `WRITE START` のログが一切出なかった。調べると、呼び出し側で `program_page_poll()` を実行していなかった。呼び出しを追加すると、ようやく最初の8バイトが送信された。

ここで「送信関数の内部が壊れている」と決めつけず、まず関数へ到達しているかを確認したことが重要だった。

### 4.2 次は `pt->count` が毎回0に戻る

ログ：

```text
WRITE START: count=0, CSR=07810805
WRITE END: count=8, CSR=07810805
POLL IN: line=0, count=8, command=3
WRITE START: count=0, CSR=07810805
WRITE END: count=8, CSR=07810805
```

`count` は8になっているのに、次のpollでは `line=0` に戻り、`pt->count = 0` が再実行されていた。

### 4.3 どこで `line` が消えるのか

pollの前後に計測点を設けた。

```text
WRITE END: count=8
poll AFTER2: line=317
poll AFTER: line=317
POLL AFTER: line=317
poll AFTER2: line=0
poll AFTER: line=0
POLL BEFORE: line=0
```

`body_func()` の終了直後には `line=317` が正しく保存されていた。統計処理の前後でも317のまま。しかし、次のProgram pollの前に**別の共通pollラッパーが呼ばれ、その時点でlineが0に戻っていた**。

### 4.4 真因：複数のProtothreadが同じ `pt->line` を解釈していた

EraseとProgramを両方毎回pollすると、Programが保存した行番号317をErase側の `switch(pt->line)` が受け取る。Erase側に対応する `case 317` がなければ、マクロの終了経路によって状態がリセットされ得る。

**解決：コマンドによるディスパッチ。**

```c
void pt_qmi_flash_poll(void)
{
    switch (pt->command) {
    case 0:
        break;  // IDLE
    case CMD_READ:
        break;  // 当時は未実装
    case CMD_ERASE:
        pt_qmi_flash_erase_4k_poll();
        break;
    case CMD_PROGRAM:
        pt_qmi_flash_program_page_poll();
        break;
    default:
        printf("unexpected command %d\n", pt->command);
        break;
    }
}
```

これで `line=317` を保持したまま `count=168 → 176 → 184 → ...` と8バイトずつ進むようになり、書き込みに成功した。

**教訓：** 同じProtothreadコンテキストを共有するなら、同時に実行する状態機械は一つだけに限定する。`PT_BEGIN()` の後にコマンド一致を検査しても、別の関数の行番号から復帰した場合には、その検査に到達する保証がない。

---

## 第5章　ハマり⑤：「10 µsで書き込み完了」の正体

### 5.1 最初の測定値

4 KiBを16ページに分けて書き込むテストで、ページごとの `time2` が10～11 µsと表示された。一方、Protothreadの状態遷移ログでは、1ページあたり約607 µsだった。

測定範囲は次のとおり。

```c
time1 = time_us_64();
if (!pt_issue_program(address + offset,
                      &flash_buffer[offset], PAGE_SIZE)) {
    print_error("Write start failed\n");
    return;
}
while (!pt_is_done())
    tight_loop_contents();
time2 = time_us_64();
message("time2: %8lld\n", time2 - time1);
```

この範囲なら、正常時にはコマンド発行からFlash書き込み完了までの時間になるはずで、10 µsは不自然だった。

### 5.2 真因：`pt->done` のクリア漏れ

`pt_issue_erase()` と `pt_issue_program()` の開始時に `pt->done = false` を設定していなかった。前回の完了フラグが残っていたため、呼び出し側の `while (!pt_is_done())` がすぐに終了していた。

**解決：** 新しいコマンドの発行時に `done=false`、完了時に `done=true` とする。

```c
// issue側（パラメータを設定し、コマンド発行）
pt->count = 0;
pt->error = 0;
pt->done = false;
pt->command = CMD_PROGRAM;

// finalize側
pt->command = 0;
pt->done = true;
```

この修正後、16ページの測定値はすべて608～609 µsとなった。

```text
time2:      608
time2:      608
...
time2:      609
time2:      609
time2:      608
```

**教訓：** 非同期APIでは「処理を開始した」ことと「前回の完了フラグが消えた」ことを同時に保証する必要がある。完了通知の初期化漏れは、処理本体が正しくても測定結果や上位層の制御を壊す。

---

## 第6章　実測結果と検証

### 6.1 1ページの状態遷移

代表的なログ：

```text
ISSUE_PROGRAM: 00380000, length = 256
START -> WRITE_BYTES: 31 us, count = 0
WRITE_BYTES -> WAIT_PROGRAM_COMP: 526 us, count = 30
WAIT_PROGRAM_COMP -> START: 51 us, count = 6
```

| 区間 | 代表値 |
|---|---:|
| START → WRITE_BYTES | 約30～31 µs |
| WRITE_BYTES → WAIT_PROGRAM_COMP | 約525～526 µs |
| WAIT_PROGRAM_COMP → START | 約50～51 µs |
| **合計** | **約606～608 µs** |

`count=30` は「その状態で継続したpollの回数」を記録する統計カウンタであり、送信したバイト数ではない。1ページ256バイトを8バイト単位で送るため、チャンク送信は32回必要となる。

### 6.2 `done` 修正後の16ページ測定

256バイト×16ページ、合計4 KiBの測定：

- 608 µs：14ページ
- 609 µs：2ページ
- 合計：**9,730 µs（9.730 ms）**
- 平均：**608.125 µs／ページ**

この時間は、`pt_issue_program()` の呼び出しから `pt_is_done()` 成立までを測定したものであり、Flashチップ内部のプログラム時間だけを意味しない。QMIへのコマンド送信、8バイト単位の送信、poll待ち、SR1確認などを含む。

### 6.3 CRC-16による読み戻し検証

テストでは4 KiBのRAMバッファを乱数で埋め、消去済みFlashにProtothread版 `ww 0` で書き込んだ。その後、RAMバッファを `0x00` で上書きし、Flashから `r 0` で読み戻してCRCを比較した。

```text
> f rand
Buffer filled randomly
> e 0
Erasing sector at 0x00380000 ...
Erase completed.
> c
CRC16: 857b
> ww 0
Writing buffer to Flash (TEST) 0x00380000 ...
Write completed.
> f 0
Buffer filled with 00.
> c
CRC16: efdf
> r 0
Reading Flash 0x00380000 ...
Read completed.
> c
CRC16: 857b
```

| 時点 | CRC-16 |
|---|---|
| 書き込み前のRAMバッファ | `0x857B` |
| RAMを `0x00` に上書きした後 | `0xEFDF` |
| Flashから読み戻した後 | `0x857B` |

**結果：** 書き込み前と読み戻し後のCRC-16が一致した。途中でRAMの内容を変更しているため、単に元のバッファを再計算しただけではないことも確認できる。ただしCRC-16には理論上の衝突があるため、最終品質確認ではバイト単位の比較も有用である。

> 注：このCRC検証ログの `ww 0` は、`pt->done` クリア漏れを修正する前の測定を含む。書き込みデータの一致は確認できたが、当時の `time2=10～11 µs` は正しい完了時間ではなかった。完了時間の確定値には修正後の608～609 µsを採用する。

---

## 第7章　設計上の要点を振り返る

### 7.1 Direct Modeの所有権を明確にする

`qmi_direct_mode_start()` / `qmi_direct_mode_stop()` は、コマンド本体の責任とする。SR1読み出しなどの下位ヘルパーが勝手にDirect Modeを開始・終了しない。

### 7.2 Protothreadの状態は操作ごとに正しく初期化する

`pt->line` は継続位置、`pt->count` は送信進捗、`pt->error` は異常情報、`pt->done` は上位層への完了通知。各変数の意味と初期化タイミングを混同しない。

### 7.3 同じ `pt->line` を複数のProtothreadに解釈させない

pollを無条件に並べて呼ぶのではなく、`pt->command` によって**実行対象を一つに限定**する。今回の不具合で最も大きな構造上の教訓となった。

### 7.4 待機条件のエラー伝播を設計する

`PT_WAIT_UNTIL_L()` の条件式が真になっただけでは、正常完了かエラーかを判別できない場合がある。エラー時に待機を抜ける設計とし、直後に `pt->error` を確認する。否定条件を使う待機ではエラー時の真偽が逆転することに注意する。

### 7.5 時間測定は「どこからどこまでか」を明示する

- 1回のpoll本体の処理時間
- 状態間の経過時間（poll間隔を含む）
- コマンド発行から完了通知までの時間

これらは別の量である。ログ出力の負荷も計測に影響する。今回、`done` のクリア漏れを発見できたのも、状態遷移ログと呼び出し元の時間測定を突き合わせたからだった。

### 7.6 Core間設計は別段階で検討する

最終方針は、Core1が共有変数経由でコマンドとパラメータを渡し、**QMI操作そのものはCore0に集約する**構成である。QMIハードウェアをCore0だけが操作する設計なら、Core間でQMI操作自体のロックを追加する必要はない。一方、コマンド受け渡し・完了通知・メモリ可視性などの同期は別途設計する必要があり、Erase / Program / ReadのProtothread化が一段落した後に検討する予定である。

なお、開発途中のテストコードにおけるCore0/Core1の実際の呼び出し配置は、最終設計と同一とは限らない。

---

## 第8章　残された課題

今回の実験で確認できたのは、主として**正常系**である。次の事項は未完了として区別する。

1. 各種待機処理のタイムアウトと異常時復旧。`qmi_wait_not_busy()`、SR1読み出し、Program完了待ちなど。
2. Direct Mode開始後に異常終了する場合のCS復旧とDirect Mode終了の確実化。
3. WRENやErase／Program命令が実際に受理されたかの異常系確認。
4. `pt->error` の詳細コード保持と、上位層へのエラー通知。
5. `read` のProtothread化。
6. Core間のコマンド受け渡しプロトコル、メモリバリア、完了通知の設計。
7. CRC-16に加えたバイト単位のデータ比較、境界値・異常系テスト。

## おわりに：ログから設計を直す

今回の開発では、Flashの命令列そのものよりも、**状態をどこで保持し、誰がその状態を更新し、上位層が何をもって完了と判断するか**が難所になった。

- `case` の位置が違えば、待機条件が再評価されない。
- Direct Modeの所有権が曖昧なら、SR1確認だけでQMIが破綻する。
- 複数の状態機械が同じ `line` を触れば、8バイトごとに先頭へ戻る。
- `done` をクリアし忘れれば、600 µs以上かかる処理が10 µsで終わったように見える。

いずれも、ログを「処理の前後」「状態遷移」「呼び出し元」に段階的に追加し、事実を積み重ねることで解決できた。最終的には256バイトのPage Programが約608 µsで安定して完了し、4 KiBの読み戻しCRC-16も一致した。

本章の結論は、**非同期Flashドライバの品質は、SPI命令の正しさだけでなく、状態遷移・所有権・完了通知の設計に支えられている**ということである。

---

### 編集メモ（同人誌化に向けて）

- 本稿は会話と提示されたログに基づく開発記録であり、ソースリポジトリの最新版を網羅的に監査したものではない。
- `pt_context_sr1_ready()` と `pt_erase_sr1_ready()` の命名は開発段階で変遷している。刊行時には掲載コードの実際の関数名に統一するとよい。
- 実測値はその時点の設定・実装・ログ条件に依存する。性能保証値として扱わない。
- `time2` の10～11 µs問題と、修正後の608～609 µsは、**同じ計測範囲でも完了フラグの初期化状態によって結果が変わった例**として対比させると、読者に伝わりやすい。
