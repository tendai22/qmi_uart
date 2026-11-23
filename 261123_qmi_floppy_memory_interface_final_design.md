# qmi_floppy 共有メモリインタフェース確定設計仕様書

- **対象**: Raspberry Pi RP2350 / pico-sdk / Z80メモリ空間 / `pt_qmi_floppy`
- **設計レビュー**: Q1～Q40 確定事項
- **版**: 1.0（2026-11-23）
- **原案**: `261123_qmi_floppy_memory_interface_design.md`
- **実装参照先**: <https://github.com/tendai22/qmi_uart.git>（main）
- **文書の位置づけ**: 実装前の確定設計。未実装の下位層改修は末尾に明示する。

## 1. 目的と対象範囲

`uint8_t mem[65536]` をZ80の64 KiBメモリ空間として使用する。Core1はZ80のメモリリード・ライトを`mem[]`に反映し、Core0は共有メモリ上の制御レジスタを監視して、既存の`pt_qmi_floppy`にREAD/WRITEを発行する。処理結果は同じ共有メモリを通してZ80へ通知する。

- **Core1**: Z80バスアクセスと`mem[]`の対応。フロッピー固有の処理は持たない。
- **Core0**: モニタ、共有メモリインタフェース、`pt_qmi_floppy`の発行・poll。
- **新設する共有メモリインタフェース層**: レジスタハンドシェイク、要求のスナップショット、パラメータ検証、CHS→LBA変換、中間セクタバッファ、結果公開。
- **既存`pt_qmi_floppy`層**: LBA単位の非同期READ/WRITE。既存の4 KiB作業バッファを保持する。
- **下位QMI Flash層**: フラッシュのREAD/ERASE/PROGRAM。

同時に処理するフロッピー要求は**最大1件**。キューは設けない。今回の同期規約はCore0/Core1の共有SRAMアクセスを対象とし、将来のPIO-DMA化に必要な同期・順序制御は別途設計する。

## 2. 共有メモリレジスタ

| アドレス | 名称 | サイズ | 意味・値 |
|---|---|---:|---|
| `0xE010` | `FLP_TRIGGER` | 1 byte | Z80が`1`を書いて要求提示。Core0が最後に`0`へ戻す |
| `0xE011` | `FLP_COMMAND` | 1 byte | `1`: READ、`2`: WRITE |
| `0xE012` | `FLP_DRIVE_NO` | 1 byte | ドライブ番号（0始まり） |
| `0xE013` | `FLP_HEAD_NO` | 1 byte | ヘッド番号（0始まり） |
| `0xE014` | `FLP_TRACK_NO` | 1 byte | トラック番号（0始まり） |
| `0xE015` | `FLP_SECTOR_NO` | 1 byte | セクタ番号（**1始まり**） |
| `0xE016`～`0xE017` | `FLP_BUFFER` | 2 bytes | `mem[]`内の転送元/転送先オフセット。バイト順は`conf.h`で指定 |
| `0xE018` | `FLP_ACK` | 1 byte | Core0が要求を受け付けると`1` |
| `0xE019` | `FLP_RESULT` | 1 byte | Core0が処理完了時に`1`。Z80が結果を確認後`0`へ戻す |
| `0xE01A` | `FLP_ERROR` | 1 byte | 処理結果のエラーコード |

制御レジスタは以下のような**8ビットvolatileアクセス**を行う。

```c
#define MEM_VOLATILE(addr) (*(volatile uint8_t *)&mem[(addr)])
```

`FLP_BUFFER`を`uint16_t *`へキャストして読み書きしない。2バイトを個別にアクセスし、`conf.h`で選択したバイト順で16ビット値を組み立てる。

## 3. エラーコード

| 値 | 定数 | 意味 |
|---:|---|---|
| 0 | `FLP_ERR_OK` | 正常終了 |
| 1 | `FLP_ERR_COMMAND` | 未定義コマンド |
| 2 | `FLP_ERR_DRIVE` | 不正ドライブ番号 |
| 3 | `FLP_ERR_CHS` | CHS範囲外、LBA変換不可 |
| 4 | `FLP_ERR_BUFFER` | バッファ範囲外、制御レジスタ領域との重複 |
| 5 | `FLP_ERR_READ` | READ実行失敗 |
| 6 | `FLP_ERR_WRITE` | WRITE実行失敗 |
| 7 | `FLP_ERR_TIMEOUT` | 下位層で明示的に検出されたタイムアウト |
| 8 | `FLP_ERR_INTERNAL` | 初期化失敗、下位層への発行拒否などの内部異常 |

## 4. ハンドシェイクの基本規約

### 4.1 Z80側

1. **`FLP_TRIGGER == 0`を待つ**。これが次の要求を提示できる条件。
2. `FLP_COMMAND`、ドライブ、CHS、`FLP_BUFFER`を設定する。
3. 必要なパラメータ設定後、`FLP_TRIGGER = 1`とする。
4. **`FLP_ACK == 1`を待つ**。ACKはCore0による受付完了を意味する。
5. **`FLP_RESULT == 1`を待つ**。READならデータ、全コマンドで`FLP_ERROR`を確認する。
6. `FLP_RESULT = 0`を書き込み、結果の受領を通知する。
7. Core0が`FLP_ACK = 0`、続いて`FLP_TRIGGER = 0`へ戻す。次の要求は`TRIGGER=0`を確認してから開始する。

Z80は要求中にパラメータと転送バッファを書き換えない。`FLP_RESULT`の初期値`0`を完了受領と誤認しないよう、Core0は**完了を公開した状態に入った後にのみ**Z80の`RESULT=0`を受領確認として扱う。

### 4.2 Core0側の5状態（Q40）

| 状態 | 処理・遷移 |
|---|---|
| `IDLE` | `TRIGGER=1`を待つ。モニタQMI処理中は受付を保留 |
| `ACCEPT` | パラメータを一度だけ取得し、`RESULT=0`・`ERROR=0`、メモリバリア後に`ACK=1`を公開 |
| `EXECUTE` | 初期化状態・引数を検証し、必要ならREAD/WRITEを非ブロッキングで進め、結果を確定 |
| `WAIT_RESULT_ACK` | `ERROR`およびREADデータの公開後、メモリバリアを経て`RESULT=1`。Z80が`RESULT=0`に戻すまで待つ（タイムアウトなし） |
| `RELEASE` | `ACK=0`、メモリバリア、**最後に`TRIGGER=0`**。`IDLE`へ戻る |

検証エラー・初期化失敗・下位層への発行失敗も、`EXECUTE`から共通の`WAIT_RESULT_ACK`へ合流する。エラー時もACKを返す。エラーを理由にACK待ちを放置しない。

### 4.3 完了結果の公開順序（Q30）

1. 下位処理が完了し、必要な完了条件を満たしたことを確認する。
2. READ成功時のみ中間バッファから`mem[]`へデータをコピーする。READ失敗時は転送先の`mem[]`を変更しない。
3. `FLP_ERROR`に結果を格納する。
4. メモリバリアを実行する。
5. **`FLP_RESULT = 1`を最後に書く**。

WRITE時はREADデータのコピーを行わない。結果の受領待ちではCore0のメインループをブロックしない。

## 5. CHSとドライブジオメトリ

ドライブごとに`TRACKS`、`HEADS`、`SPT`（1トラック当たりのセクタ数）、論理セクタサイズ（128または256バイト）を`conf.h`でコンパイル時設定する。ドライブ番号、ヘッド、トラックは0始まり、セクタ番号は1始まり。

```text
LBA = ((track * HEADS) + head) * SPT + (sector - 1)
```

検証条件：

- `0 <= track < TRACKS`
- `0 <= head < HEADS`
- `1 <= sector <= SPT`
- `TRACKS * HEADS * SPT > 0`
- `TRACKS * HEADS * SPT <= drive_size[drive]`
- 変換したLBAが当該ドライブの範囲内であること

フラッシュの全容量をジオメトリで使い切る必要はない。ジオメトリの整合性検証は**新設する共有メモリインタフェース層の初期化**で実施し、既存`pt_qmi_floppy_init()`の責務を変更しない。

## 6. パラメータ検証（Q35）

Core0は`ACCEPT`で取得したスナップショットに対して、次の**固定順序**で検証する。最初の異常を報告し、以降の検証・QMI操作は実施しない。

| 順位 | 検証 | エラー |
|---:|---|---|
| 1 | インタフェース初期化済みか | `FLP_ERR_INTERNAL` |
| 2 | READ(1)/WRITE(2)のいずれか | `FLP_ERR_COMMAND` |
| 3 | ドライブ番号が有効か | `FLP_ERR_DRIVE` |
| 4 | CHSおよびLBAが有効か | `FLP_ERR_CHS` |
| 5 | 転送バッファが有効か | `FLP_ERR_BUFFER` |

転送は**1コマンドにつき1論理セクタ**。バッファ開始アドレス`addr`と長さ`size`について、`addr + size <= 65536`であること（64 KiBをまたいで折り返さないこと）、および転送領域が制御レジスタ`0xE010`～`0xE01A`と重ならないことを検証する。加算のオーバーフローを避けられる整数幅で検証する。

## 7. READ/WRITEの処理とバッファ

新設インタフェース層は128/256バイトの**中間セクタバッファ**を所有する。既存`pt_qmi_floppy`の**4 KiB `work_buffer`**とは別のバッファである。

### READ

1. パラメータを検証し、CHSをLBAへ変換。
2. `pt_issue_floppy_read(drive, lba, intermediate_buffer)`を発行。
3. `pt_qmi_floppy_poll()`を繰り返し呼ぶ。
4. 下位層が正常終了した場合だけ、中間セクタバッファから`mem[FLP_BUFFER...]`へコピー。
5. `FLP_ERROR`を設定し、メモリバリア後に`FLP_RESULT=1`。

### WRITE

1. パラメータを検証し、CHSをLBAへ変換。
2. **発行前に**`mem[FLP_BUFFER...]`から中間セクタバッファへ1論理セクタをコピー。
3. `pt_issue_floppy_write(drive, lba, intermediate_buffer)`を発行。
4. `pt_qmi_floppy_poll()`を繰り返し呼ぶ。
5. `FLP_ERROR`を設定し、メモリバリア後に`FLP_RESULT=1`。

WRITEでは下位層が使用する中間バッファを、完了まで維持する。READ失敗時にはZ80側の転送先データを変更しない。

## 8. 既存`pt_qmi_floppy` APIとの接続（Q38・Q39）

参照する既存API：

```c
bool pt_issue_floppy_read(int drive, uint32_t lba, uint8_t *buffer);
bool pt_issue_floppy_write(int drive, uint32_t lba, uint8_t *buffer);
void pt_qmi_floppy_poll(void);
bool pt_floppy_is_done(void);
bool pt_floppy_issue_ready(void);
int  pt_floppy_error_code(void);  // 読み出すとエラー値をクリア
```

- `pt_issue_floppy_read/write()`が`false`を返した場合、`FLP_ERR_INTERNAL`。発行済みとしてpoll待ちしない。
- 発行成功後は`pt_qmi_floppy_poll()`を繰り返し呼び、**`pt_floppy_is_done()`と`pt_floppy_issue_ready()`の両方が真**になるまで結果を公開しない。
- `is_done()==true`でも`issue_ready()==false`ならpollを継続する。
- 両条件成立後、`pt_floppy_error_code()`を**1トランザクションにつき1回だけ**呼び出し、ローカル変数へ保存する（read-and-clear API）。
- 下位READ/WRITE実行失敗はそれぞれ`FLP_ERR_READ`/`FLP_ERR_WRITE`に変換する。
- 下位層が**明示的にタイムアウトを識別できるよう改修された後**は、`FLP_ERR_TIMEOUT`として伝搬する。

`pt_qmi_floppy`の発行とpollはCore0に集約する。既存のクロスコア向け原子操作があっても、新設層で複数の同時発行を許可しない。

## 9. Core0モニタとの共存（Q33など）

- Core0メインループはフロッピー状態機械を非ブロッキングで進める。
- モニタはTinyUSB CDCから入力を受け取り、モニタコマンドを同期的に1件処理してメインループへ戻る。処理中はフロッピー受付・pollが遅れる可能性がある。
- `FLP_TRIGGER=1`（Z80の結果受領待ちを含む）の間、通常のモニタQMIコマンド（READ/ERASE/WRITE等）は**即時拒否**し、キューへ入れない。
- 逆に、モニタQMI操作の実行中にZ80が`TRIGGER=1`を提示した場合、**モニタ操作終了後**にCore0が要求を受け付け、`ACK=1`を返す。Z80はそれまでACKを待つ。
- 長時間のモニタQMI操作とZ80プログラム実行の同時利用は今回の想定ユースケースから除外し、ACK待ちに専用タイムアウトを設けない。
- 非常脱出コマンドは通常の排他制約の例外とする（詳細は§11）。

### モニタによる`mem[]`へのイメージ展開

- 起動時またはモニタの`x`コマンドで、フラッシュ上のIntel HEX / Motorola S-recordイメージを`mem[]`へ展開する。
- HEX/S-recordは**1行を1モニタコマンド**として処理し、拡張アドレスなどのパーサ状態を行間で保持する。
- どちらの形式も`0xE010`～`0xE01A`への書き込みを**スキップ**する。ただし入力レコードのチェックサム検証には全バイトを用いる。
- 通常モニタモードではZ80をRESET状態に保持するため、イメージ展開とZ80のフロッピー要求は競合しない。
- デバッグのbreak/stepモードでのロードは、Z80が停止しており、`TRIGGER=0`の場合だけ許可する。`TRIGGER=1`なら禁止する。
- 行単位のモニタコマンド間ではフロッピーpollが実行される。

## 10. 起動時・初期化失敗時（Q31・Q32・Q36）

### 10.1 初期化順序

1. `sector_size[]`を設定する。
2. `pt_qmi_floppy_init()`を実行する。
3. 新設する共有メモリインタフェース層を初期化する。
4. `conf.h`のCHSジオメトリとドライブ容量の整合性を検証する。
5. 制御レジスタ11バイトをすべて0にし、Core1/Z80の開始前に初期状態を確立する。
6. 初期化成功時のみ実際のフロッピーREAD/WRITE発行を有効にする。

起動時の制御レジスタは`FLP_TRIGGER`、`FLP_COMMAND`、`FLP_DRIVE_NO`、`FLP_HEAD_NO`、`FLP_TRACK_NO`、`FLP_SECTOR_NO`、`FLP_BUFFER`の2バイト、`FLP_ACK`、`FLP_RESULT`、`FLP_ERROR`の**計11バイトをすべて0**とする。`TRIGGER`は最後に0へ公開する。

### 10.2 初期化失敗

- Core0モニタは停止せず、診断メッセージを出す。
- 実際のフロッピーREAD/WRITEを無効にし、`pt_issue_floppy_read/write()`を呼ばない。
- ただし共有メモリのハンドシェイク処理は継続する。
- Z80から要求が来たら通常どおり`ACK=1`を返し、`FLP_ERR_INTERNAL`、`RESULT=1`で異常終了を通知し、Z80の結果受領後に解放する。

「フロッピーインタフェース無効」は**実処理の無効化**を指し、ACK/RESULT応答の停止を意味しない。

## 11. 非常脱出とリセット（Q25・Q36）

非常脱出は、進行中のQMIハードウェア処理を強制中断しない。Z80のRESETは別のモニタコマンドであり、非常脱出前に実施することを推奨する。

1. Z80が停止（RESET状態）していることを確認する。
2. 新規要求の受付を停止する。
3. 進行中の`pt_qmi_floppy`処理を通常のpollで**最後まで完了**させる。WRITEの場合、ERASE/PROGRAMを含む残りの処理を完了させる。
4. `pt_floppy_issue_ready()`が真であり、QMIがアイドル状態であることを確認する。必要なら専用のインタフェースリセットAPIを設ける。
5. 制御レジスタ11バイトを0に初期化する。`FLP_ACK=0`を書いた後にメモリバリアを実行し、**`FLP_TRIGGER=0`を最後に公開**する。
6. 以後、通常のアイドル状態に復帰する。

非常脱出は共有メモリの制御状態をリセットするものであり、**フロッピー領域の消去・フォーマットではない**。下位層が永久に応答しない故障までソフトウェア復旧を保証しない。その場合はハードリセットまたは電源再投入を許容する。

## 12. 共有メモリ可視性とアクセス規約（Q37）

- 制御レジスタは`MEM_VOLATILE(addr)`で8ビットアクセスする。
- 複数バイトのパラメータは、`TRIGGER=1`を受け付けた後に一度だけスナップショットする。Z80側は要求中に変更しない。
- `RESULT/ERROR`の初期化後、メモリバリアを挟んで`ACK=1`を公開する。
- READデータと`ERROR`の確定後、メモリバリアを挟んで`RESULT=1`を公開する。
- `ACK=0`の後にメモリバリアを挟み、`TRIGGER=0`を最後に公開する。
- 実装時はCPUのメモリ順序だけでなく、コンパイラの並べ替えも含めて、`__dmb()`および適切なコンパイラバリア／原子操作の適用を確認する。
- 将来のPIO-DMAの同期、可視性、順序保証は今回の設計範囲外とする。

## 13. テスト観点

| 分類 | 主な確認内容 |
|---|---|
| ハンドシェイク | `TRIGGER→ACK→RESULT→RESULTクリア→ACKクリア→TRIGGERクリア`の順序、1件のみ受付 |
| READ | 正常データ、128/256バイト、READ失敗時に転送先不変 |
| WRITE | 発行前スナップショット、1論理セクタ単位、下位完了まで中間バッファ維持 |
| CHS | 先頭/末尾、sector=0、範囲外head/track/sector、容量超過 |
| バッファ | 64 KiB末尾境界、折り返し、制御レジスタとの重複 |
| エラー | 未定義コマンド、ドライブ不正、複数不正時の優先順位、発行拒否、READ/WRITE失敗 |
| 完了API | `is_done`と`issue_ready`の両立、`error_code`の一回取得 |
| 初期化 | 成功・失敗、失敗時の`FLP_ERR_INTERNAL`応答 |
| モニタ | QMI排他、ACK受付遅延、HEX/S-recordの制御レジスタ保護 |
| 非常脱出 | Z80停止、進行中WRITE完了待ち、QMI idle、レジスタ初期化順序 |
| 同期 | Core0/Core1間の公開順序、volatile/メモリバリア |
| タイムアウト | 下位層改修後に`FLP_ERR_TIMEOUT`が区別して伝搬すること |

## 14. 実装前に区別しておく事項

### 14.1 設計として確定したこと

- 新設層の責務、レジスタ配置、5状態のハンドシェイク、CHS変換、バッファ方針、エラー体系、モニタ排他、初期化失敗応答、非常脱出手順、完了APIの利用順序は**Q1～Q40で確定**。

### 14.2 後続の実装・検証作業

1. **Q29: 下位層のタイムアウト処理の点検・改修**。`pt_qmi_flash`と`pt_qmi_floppy`でタイムアウトを通常のREAD/WRITEエラーと区別し、`FLP_ERR_TIMEOUT`へ伝搬できるようにする。下位層の具体的な列挙子・API変更は、現行ソースを確認してから決定する。
2. 新設共有メモリインタフェース層の実装（5状態、検証、CHS変換、セクタバッファ）。
3. Core0メインループ・モニタとの統合。
4. 正常系・異常系・Core0/Core1統合試験。
5. PIO-DMA化時のメモリ同期規約は**別設計**。

本書は実装済みであることを意味しない。特にタイムアウト識別の下位層改修は**設計確定・実装保留**である。

---

## 付録A. 設計レビュー後半の決定記録

| 質問 | 確定事項 |
|---|---|
| Q30 | READデータ・ERROR確定後、メモリバリア、RESULT=1 |
| Q31 | 初期化失敗でもCore0モニタ継続 |
| Q32 | 初期化失敗でもACK/RESULT応答し、INTERNALを返す |
| Q33 | モニタQMI処理終了後にTRIGGERを受付。Z80はACK待ち |
| Q34 | 未定義コマンドにもACKし、COMMANDエラーで終了 |
| Q35 | 初期化→コマンド→ドライブ→CHS→バッファの順に検証 |
| Q36 | 起動時・非常脱出時は制御レジスタ11バイトを0に初期化 |
| Q37 | 8ビットvolatileアクセス、バリア、PIO-DMA同期は別設計 |
| Q38 | 下位発行拒否はINTERNAL、実行失敗はREAD/WRITE |
| Q39 | is_doneとissue_readyを確認し、error_codeを1回だけ取得 |
| Q40 | IDLE/ACCEPT/EXECUTE/WAIT_RESULT_ACK/RELEASEの5状態 |
