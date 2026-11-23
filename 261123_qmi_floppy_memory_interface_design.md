# qmi_floppy 機能のメモリインタフェース設計

RP2350, pico-sdk, Core0, Core1間のインタフェースとして、添付文書のメモリインタフェースの考え方を、qmi_floppy ドライバに適用したいです。

Core1がZ80バスアクセスに対応し、Z80メモリリードライトで以下のメモリの読み書きを通じて、Core0 が pt_issue_floppy_read/write コマンドを発行し、処理が完了したらメモリを介して Z80側に完了とエラーコードを伝えるという機能です。

- `uint8_t mem[65536]` をZ80の64 KiBメモリ空間として用いる。
- Core0とCore1は同じオンチップSRAMを参照する。
- pt_qmi_floppy処理はCore0に集約する。

# 3. 共有メモリのレジスタ配置と Core0/Core1インタラクション

```c
#define FLP_TRIGGER   0xe010
#define FLP_COMMAND   0xe011
#define FLP_DRIVE_NO  0xe012
#define FLP_HEAD_NO   0xe013
#define FLP_TRACK_NO  0xe014
#define FLP_SECTOR_NO 0xe015
#define FLP_BUFFER    0xe016 // two bytes
```

(uint16_t *)mem[FLP_BUFFER] にバッファアドレス(mem配列内のオフセット)、ドライブ、ヘッド、トラック、セクタ番号を設定して、FLP_COMMANDに1(READ),2(WRITE)を設定してから FLP_TRIGGER を 0 -> 1 に設定すると、Core0の qmi_floppyループで pt_issue_floppy_read or write を発行して実行を開始する。

```c
#define FLP_ACK       0xe018
#define FLP_RESULT    0xe019
```

実行完了すると、Core0 qmi_floppyループで、mem[FLP_RESULT] を 0 -> 1 とする。
Z80側は mem[FLP_RESULT] を監視して、1になった時点でコマンド実行が完了したと知る。mem[FLP_RESULT] を読み出してから、Z80は mem[FLP_RESULT] を 1 -> 0とする。

Core0側では、mem[FLP_RESULT] が 0になった時点でコマンドトランザクションが完了したと知り、mem[FLP_TRIGGER] を 1 -> 0 とする。

```c
#define MEM_VOLATILE(addr) (*(volatile uint8_t *)&mem[(addr)])
```

各メモリは volatile 宣言しておき、コンパイラが最適化しすぎて実アクセスコードを消さないようにする。

実メモリへの書き込み順序保証 __dmb() は必要に応じて挿入してください。
