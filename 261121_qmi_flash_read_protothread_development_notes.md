# RP2350 QMI Flash Read のProtothread化 — 設計・実装・デバッグ・検証記録

> 年末同人誌向けの開発記録素材。2026年11月の対話で提示されたコードと実機ログをもとに構成。完成原稿ではなく、事実・観察・推測・今後の課題を分けた一次メモ。

## 1. 背景と目標

Raspberry Pi Pico 2（RP2350）から外付けSPI NOR Flash（Winbond W25Q32RV）へQMI Direct Modeでアクセスする。4 KiBセクタ消去（Erase）、256バイトページ書き込み（Program）に続き、4 KiB読み出し（Read）をProtothreadの`start/poll`型に移す。将来はCore0がQMIを所有し、Core1側から共有コマンドを発行する構成へ進める予定である。

今回の検証対象は、Flashアドレス`0x00380000`からの4 KiB読み出し。従来のReadコマンド`0x03`を使用し、1回のpollで最大8バイトを処理する。

## 2. Read APIと共通コンテキスト

### コマンド発行

```c
bool pt_issue_read(uint32_t address, uint8_t *buffer, uint32_t length) {
    if (pt->command)
        return false; // 他のコマンド実行中
    pt->command = CMD_READ;
    pt->address = address;
    pt->buffer = buffer;
    pt->length = length;
    pt->count = 0;
    // pt->line = 0; // 正常完了時のPT_ENDで初期化される前提
    pt->done = 0;
    pt->error = 0;
    debug("ISSUE_READ: %08lx, length = %ld\n",
          pt->address, (unsigned long)pt->length);
    return true;
}
```

`pt->command`で多重発行を拒否し、`pt->done`を発行時にクリアする。これは先行するErase/Programのデバッグで判明した重要なルールである。`pt->line`はProtothreadの再開位置であり、前回処理が`PT_END`まで到達して0に戻っていることを前提としている。

### パラメータチェック

```c
bool pt_read_param_check(void) {
    if (pt->buffer == NULL) return false;
    if (pt->length == 0) return false;
    if (pt->address >= FLASH_SIZE) return false;
    if (pt->length > FLASH_SIZE - pt->address) return false;
    return true;
}
```

最後の条件は、`address + length`の加算によるオーバーフローを避けながらFlash末尾を超える要求を排除する。ReadにはPage Programのような256バイト境界制約はない。

## 3. 1回のpollで最大8バイト読む

提示された実装の中心は次の関数である。

```c
static bool pt_read_chunk_bytes(void) {
    int n = pt->length - pt->count;
    if (n > 8) n = 8;
    for (int i = 0; i < n; ++i) {
        // ダミーデータ送信でクロックを発生させる
        qmi_hw->direct_tx = 0xFF;

        if (!qmi_wait_busy_clear()) {
            pt->error = 6;
            return true;
        }

        uint32_t start = time_us_32();
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS) {
            if ((uint32_t)(time_us_32() - start) > FLASH_READ_TIMEOUT_US) {
                pt->error = 7;
                return true;
            }
        }
        pt->buffer[pt->count++] = (uint8_t)qmi_hw->direct_rx;
    }
    return pt->count >= pt->length;
}
```

ここでの戻り値は「8バイト読み終わったか」ではなく、**要求した全データの受信が完了したか**を意味する。未完了なら`false`を返して次のpollへ進む。エラー時に`true`を返すのは、待機マクロから抜けて直後の`pt->error`判定へ制御を渡すためである。単純に`false`を返すとエラーが発生しても待機を続けてしまう。

設計上の注意：この関数は「最大8バイトに処理を分割」しているが、各バイトの送受信待ちは関数内部のビジーウェイトであり、**完全にノンブロッキングな1バイト転送ではない**。1回のpollに要する時間には上限待ちが含まれる。

## 4. ReadのProtothread本体

コード上の流れは以下の通り。

1. `PT_BEGIN(pt)`、コマンド種別の確認。
2. 引数検証、`qmi_direct_mode_start()`。
3. `PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(), WAIT_PREV_CMD_COMPLETE_IN_READ)`で前コマンドの完了を待つ。
4. CS0をアサートし、`CMD_READ_DATA`（`0x03`）と24ビットアドレスを送る。
5. `pt->count=0`にして、`PT_WAIT_UNTIL_L(pt, pt_read_chunk_bytes(), READ_BYTES)`で4 KiBを分割受信する。
6. QMIのBUSY解除待ち、CS解除、Direct Mode停止。
7. `pt->command=0; pt->done=true; PT_END(pt)`で完了を通知する。

重要なコード断片：

```c
PT_WAIT_UNTIL_L(pt, pt_context_sr1_ready(), WAIT_PREV_CMD_COMPLETE_IN_READ);
if (pt->error)
    goto error;

qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
if (!qmi_flash_write_byte(CMD_READ_DATA) ||
    !qmi_flash_write_byte((uint8_t)(pt->address >> 16)) ||
    !qmi_flash_write_byte((uint8_t)(pt->address >> 8)) ||
    !qmi_flash_write_byte((uint8_t)pt->address)) {
    goto error;
}

pt->count = 0;
PT_WAIT_UNTIL_L(pt, pt_read_chunk_bytes(), READ_BYTES);
if (pt->error)
    goto error;
```

エラー経路ではQMI BUSY待ち、CS解除、Direct Mode停止を試み、未設定なら汎用エラーコード1をセットする。発行時に`done=0`、終了時に`done=true`とすることが外側の完了待ちの成立条件である。

### 共通ディスパッチ

```c
void pt_qmi_flash_poll(void) {
    switch (pt->command) {
    case 0: break;
    case CMD_READ:    pt_qmi_flash_read_poll(); break;
    case CMD_ERASE:   pt_qmi_flash_erase_4k_poll(); break;
    case CMD_PROGRAM: pt_qmi_flash_program_page_poll(); break;
    default:
        printf("unexpected command %d\n", pt->command);
        break;
    }
}
```

**同じ`pt->line`を使う複数のProtothread本体を無条件に呼ばない**ことが先行するProgramの不具合から得た教訓。現在のコマンドに対応する本体だけを呼ぶ。

## 5. 実測ログ：4 KiB読み出し

`x 0`でデバッグバッファをクリアし、`rr 0`を実行した結果：

```text
> rr 0
Reading buffer to Flash (TEST) 0x00380000 ...
time2:     8271
Write completed.
> x
flush_buffer:ISSUE_READ: 00380000, length = 4096
START -> READ_BYTES: 28 us, count = 0
READ_BYTES -> START: 8243 us, count = 510
```

状態遷移時間の合計は`28 + 8243 = 8271 µs`であり、外側の経過時間`time2=8271 µs`と一致する。後続テストでは`8309 µs`も観測された。読み出し全体の実効速度は8271 µsのケースで約495 kB/s（約484 KiB/s）。これはソフトウェア待ち・状態管理を含む実測値であり、SPI線上の理論速度ではない。

### `count=510`の読み方

4 KiBを8バイト単位に分けると、チャンク関数の呼び出しは通常`4096/8=512`回となる。一方ログの`count=510`は、**データバイト数を示す`pt->count`ではなく、状態遷移計測器側のpoll集計値**である。最初と最後のpollを遷移側として数える集計方式なら510という値と整合する。ただし、集計器`pt_qmi_flash_func_poll()`の実装をここでは再掲していないため、差の厳密な原因は計測コードの確認事項として残す。`pt->count`が4096に達したかを直接記録すれば、両者を明確に区別できる。

## 6. データ検証：固定パターンと乱数

### 6.1 `0xA5`固定パターン

```text
> f a5
> ee 0
> ww 0
> c
CRC16: 4ee4
> f ff
> c
CRC16: 0fe1
> rr 0
> c
CRC16: 4ee4
> d 0
0000: A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5
...
00F0: A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5 A5
```

RAMバッファを意図的に`0xFF`へ変更し、CRCが変化した後にReadして元のCRCへ戻ることを確認した。ダンプでは先頭256バイトの`0xA5`を目視確認。

### 6.2 Erase直後の読み出し

```text
> ee 0
Erasing (TEST) sector at 0x00380000 ...
error: 0
time2:    32899 ...
Erase completed.
> rr 0
...
> d 0
0000: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
...
00F0: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
```

先頭256バイトが消去値`0xFF`であることを目視確認。4 KiB全体の消去確認については、全域比較またはCRC検証を追加すればより厳密になる。

### 6.3 乱数パターン：4 KiB全体のCRC一致

```text
> f rand
Buffer filled randomly
> c
CRC16: 857b
> ww 0
Writing buffer to Flash (TEST) 0x00380000 ...
time2:      857
... （計16ページ、各857 us）
Write completed.
> f ff
Buffer filled with FF.
> c
CRC16: 0fe1
> rr 0
Reading buffer to Flash (TEST) 0x00380000 ...
time2:     8309
Write completed.
> c
CRC16: 857b
```

書き込み前と読み戻し後のCRC-16は`0x857B`で一致。途中でRAMを`0xFF`に変更しているため、単にRAM上の書き込み元データを再検査したのではないことも確認できる。CRC-16には理論上衝突があり、最終的な厳密検証ではバイト単位比較も有効。

## 7. Erase / Program / Read の処理時間比較

同一系列の後続テストで観測された値：

| 操作 | サイズ | 観測時間 | 備考 |
|---|---:|---:|---|
| Erase | 4 KiB | 32,899 µs | `ee 0` |
| Program | 256 B | 857 µs | 16ページとも857 µs |
| Program | 4 KiB | 13,712 µs | 857 × 16、各ページ計測値の合計 |
| Read | 4 KiB | 8,309 µs | `rr 0` |

別の実行ではProgramが608～609 µs/ページ、Readが8271～8272 µs/4 KiBだった。Program時間の変動原因は提示ログだけでは確定できない。Flash内部処理、計測条件、コード差分などを分離して調べる余地がある。

## 8. Read実装で見つかった改善候補

以下は**今回の実機テストで不具合が発生したという意味ではなく、コードレビュー上の改善候補**。

1. **エラーコードを正常終了時に消さない。** `done:`で`qmi_direct_mode_stop()`成功時に`pt->error=0`を代入する必要はない。発行時に0にし、以後はエラー時だけ設定する方針をErase/Program/Readで統一する。
2. **TX FIFOの空き確認。** `qmi_hw->direct_tx=0xFF`の前にFIFO空きを確認する余地がある。現状は各バイトのBUSY解除を待っており正常動作している。
3. **型の整合。** `int n = pt->length - pt->count;`は符号なし値から`int`への変換を伴う。大きなRead長まで一般化するなら`uint32_t`などへ統一する。
4. **表示とコメントの修正。** `rr 0`の出力が`Reading buffer to Flash`および`Write completed.`となっている。Read用表示に修正する。ソース中の`// Read: 02h ...`は`03h`、`// start erase`はRead用のコメントに修正する。
5. **異常系とタイムアウト。** `qmi_wait_busy_clear()`やRXEMPTY待ちのタイムアウトを注入して、`pt->error`、CS解除、Direct Mode停止、`done`通知を確認する。
6. **Protothread復帰の前提。** `pt->line`を発行時に明示的に0へ戻していないため、通常の`PT_END`で0になること、異常経路も`PT_END`へ到達することを確認する。

## 9. 同人誌向けの「ハマりと解決」整理

Read単独の開発では、提示されたログ上は大きな停止・データ破損のハマりは報告されていない。Readが比較的素直に動いた背景には、先行するErase/Programで次の問題を解決していたことがある。

- **`pt->line`の意図しないリセット：** 同一コンテキストで別のProtothread本体を呼ぶと再開位置が壊れる。`pt->command`によるディスパッチへ変更。
- **`pt->done`のクリア漏れ：** 新コマンド発行時に前回の完了フラグが残り、待機ループが即座に終了。Programの見かけ上の所要時間が約10 µsとなった。発行時に`done=false`へ修正すると約608 µsとなり、内部計測と一致。
- **Direct Modeの所有権：** 共通SR1確認ヘルパーの内側でDirect Modeを重ねて開始しない。外側の処理が開始・停止を管理する。
- **待機条件でのエラー伝搬：** `PT_WAIT_UNTIL_L`から抜けるため、エラー時にも条件関数が`true`を返し、直後に`pt->error`を判定する。

Readの開発記録は「Erase/Programで培った状態管理規則を適用した結果、4 KiB Readを少数の状態で実装できた」という流れでまとめると、開発の連続性が伝わる。

## 10. 到達点と次の段階

現時点の正常系として、Protothread版Erase・Program・Readを組み合わせ、4 KiBの固定パターンおよび乱数パターンについて書き込み・読み戻しのCRC-16一致を確認した。Readの状態遷移計測は外側の時間測定と整合した。

次の段階は、異常系の検証、必要なエラー回復処理の整備、そしてCore0がQMIを専有する構成でのCore1とのコマンド・完了通知インタフェース設計である。コア間同期は今回のRead正常系検証の範囲外であり、別途設計する。

---

### 原稿化の際の編集メモ

- 「正常動作を確認した事実」と「改善候補」を混同しない。
- `count=510`の厳密な説明はpoll計測器のコードを再確認してから確定する。
- Flash読み出しの性能比較を掲載する場合は、QMIクロック、CPUクロック、最適化オプション、計測範囲を明記する。
- 固定パターン、Erase後の`0xFF`、乱数CRC一致を別々の検証として見せると説得力が増す。
- 本メモは対話中に提示されたコードとログを基礎としており、リポジトリの最新状態との自動照合は行っていない。
