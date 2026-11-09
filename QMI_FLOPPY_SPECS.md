# qmi_floppy プロジェクト

※ 開始時の指示テキスト、まずこれを ChatGPT (Plus契約)にアップロードしてコードを作ってもらった。

本プロジェクトは、すでに開発した「Raspberry Pi Pico2内蔵フラッシュアクセスルーチン」を使用して、レトロCPU用の仮想フロッピードライブ機能を開発するプロジェクトである。


## 対象デバイス 

* MPU: RP2350
* Flash: Winbond W25Q32RV

## 主要資料

* ソースコード: github.com/tendai22/qmi_uart.git 直下の
  + qmi_flash.[ch]
  + qmi_flash_async.[ch]
  + cmd.[ch] 
* pico-sdk ソースコード
* Raspberry Pi Pico 公式サンプルコード
* 一般的な組み込み知識

* cmd.[ch] は qmi_floppy_read/writeのデバッグのための単独コマンドを追加する。

## 資料の優先順位

回答時は以下の優先順位を守ること

1. pico-sdk ソースコード
2. Raspberry Pi Pico 公式サンプルコード
3. ソースコード: github.com/tendai22/qmi_uart.git 直下の
  + qmi_flash.[ch]
  + qmi_flash_async.[ch]
  + cmd.[ch] 
4. 一般的な組み込み知識

上位資料と下位資料が矛盾する場合は、上位資料を優先すること。

## 回答方針

回答は以下の区分で記述すること。

### 【仕様】

データシート、リファレンスマニュアル、
回路図に記載された事実。

### 【設計判断】

仕様から導かれる推奨設計。

### 【推測】

資料に明示記載がなく、
推論を含む内容。

### 【検証方法】

実機またはコードにより確認可能な方法。

## 根拠提示

仕様に基づく回答では可能な限り以下を示すこと。

- 資料名
- セクション番号
- ページ番号

## プロジェクトの目的

レトロCPU用仮想フロッピードライブ機能を提供するライブラリを開発する。

qmi_flash.h, qmi_flash_async.h が提供するPico flash R/E/P 
機能を使用し、フロッピーのセクタリードライト機能を実現する。

目的は学習で泣く実装である。

## 要求仕様

* 以下で、address は仮想フロッピーのトラック0, 面0, セクタ1を開始位置として
  シーケンシャルに換算したセクタ番号とします。ドライブごとにセクタサイズ(バイト数、通常128 or 256)を別途保持するグローバル配列 uint32_t sector_size[drive] から得るものとします。
* uint32_t sector_size[drive], uint32_t drive_size[drive] はグローバル変数で割り当てます。
* この換算は済ませて換算済の address が渡されるものとします。
* ドライブの大きさ(address の上限)は、uint32_t drive_size[drive] で与えられるものとします。
* Pico Flash (通常 4MByte)の上端から各ドライブのサイズだけ前にさかのぼり、各ドライブの先頭を決めます。
  この各ドライブ先頭のアドレスに、引数で渡されたセクタ数 * sector_size[drvie] を加えたものが qmi_flash
  で渡すアドレスとします。
* read/write関数の詳細仕様は最後に示す。
* また、最大ドライブ数は2個とする。NUM_DRIVE マクロで定義、参照してください。

## 開発方針

回答時は以下を考慮すること。

性能
RAM使用量
FLASH使用量
割込み安全性
保守性
テスト容易性

必要に応じてコード例を積極的に提示すること。

コーディング方針
言語
C99
開発環境
Pico SDK
原則
動的メモリ確保禁止
malloc/free 使用禁止
グローバル変数の最小化
static の適切な利用
マジックナンバー禁止
エラーコードを返す
再入可能性を考慮する
busy wait を使用する場合は理由を示す
コードレビュー形式

コードレビュー時は以下の順序で回答すること。

問題点
リスク
改善案
修正版コード

## 開発ステップ

1. 同期式read/write関数を作成する。
2. それを cmd.c のコマンドインタプリタに組み込み、動作確認する。
3. read/write 関数のテストケースを開発する。要件、仕様は2の後で提示する。
3. 非同期式read/write関数を作成する(この要件・仕様は2の後で提示する)
4. それを cmd.c のコマンドインタプリタに組み込み、動作確認する。
6. 3で開発したテストケースを通し、全部Okとなった時点で本プロジェクトを完了とする。

## read/write関数の仕様

* read操作: qmi_floppy_read_sector関数、write操作: qmi_floppy_write_sector関数が行う。これら関数の詳細仕様は
  以下 qmi_floppy.h の説明中で説明する。
* read操作、write操作中にread/write操作を呼び出した場合は、そのリクエストを受け付けず、前の実行中の操作を継続する。
* read/write操作中かどうかを知るために、floppy_operation に Read(1)/Write(2)を代入する。
* read/write操作が完了すると、floppy_operation に 0 を代入する。
* read/write呼び出し側は、floppy_operation が 0 であることを確認して qmi_floppy_read/qmi_floppy_writeを呼び出す。
* read/write操作の結果は両関数の返し値 qmi_floppy_result_t enum型で返す。
  QMI_FLOPPY_RESULT_OK(0), QMI_FLOPPY_RESULT_ERROR(1)とする。エラー種別を増やす場合はこの enum型の定義を増やす。
* qmi_floppyでの長さ単位は、セクタ(128 or 256バイト)とする。
- sector_size[drive]：1セクタのバイト数（128または256）
- drive_size[drive]：ドライブの総セクタ数
- drive_size[drive] = DRIVE_A_SIZE / sector_size[drive]（Drive Bも同様）
qmi_floppyシステムでは、長さは原則セクタ数で指定するものとします。

qmi_floppy.h で定義する関数
以下の２関数は、同期式とする。read/write operation が完了するまでリターンしなくてよい。今のところは。

```c
typedef enum {
    QMI_FLOPPY_RESULT_OK = 0,
    QMI_FLOPPY_RESULT_ERROR
} qmi_floppy_result_t;

// Read
extern qmi_floppy_result_t
qmi_floppy_read(int drive, uint32_t address, int count, uint8_t *buffer);

// Write
extern qmi_floppy_result_t
qmi_floppy_write(int drive, uint32_t address, int count, uint8_t *buffer);
```

## 下位機能: qmi_flash.hの仕様

```c
// 非同期(コマンド発行/ポーリング分割) API
// Read
extern bool qmi_flash_read_start(uint32_t address, uint8_t *buffer, size_t length);
extern qmi_flash_poll_result_t qmi_flash_read_poll(void);
// Read stable
extern bool qmi_flash_read_start_stable(uint32_t address, uint8_t *buffer, size_t length);
extern qmi_flash_poll_result_t qmi_flash_read_poll_stable(void);

// Erase
extern bool qmi_flash_erase_4k_start(uint32_t address);
extern qmi_flash_poll_result_t qmi_flash_erase_4k_poll(void);

// Program
extern bool qmi_flash_program_start(uint32_t address, const uint8_t *buffer, size_t length);
qmi_flash_poll_result_t qmi_flash_program_poll(void);

```

Read/Erase/Programとも start関数で処理を開始し、poll関数がPOLL_FLASH_POLL_BUSYを返している間はpoll関数を繰り返し呼び出し、DONEを返せば完了、ERRORを返せば失敗とみなす。

start関数がtrueならコマンド開始成功、以後 pollを連続して呼び出す。false ならコマンド開始失敗とみなし、poll関数を呼び出してはならない。

```
typedef enum
{
    QMI_FLASH_POLL_BUSY = 0,
    QMI_FLASH_POLL_DONE,
    QMI_FLASH_POLL_ERROR
} qmi_flash_poll_result_t;
```

Read(任意バイトの読み出し)、ERASE(4kバイトセクタの消去)、Program(256バイトページ倍数の書き込み)の3機能を提供する。Program中で事前に eraseすることはないので、qmi_drive_writeでは、内部4kバッファにread->バッファを部分書き換え->erase->write 16ページの順で処理して欲しい。今回のWriteでは、program_page呼び出しで、lengthは1ページ256バイト固定で呼び出してほしい。

まずはここまでチャットに貼り付ける。必要があれば、qmi_flash.hをそのまま貼り付けて他の定義を示してもよいので必要なら言ってください。

qmi_flash には USE_STUB マクロでスタブ機能が利用できる。これは、SRAM上にフロッピーイメージを確保し、そこで Read/write機能の動作確認を行う。
USE_STABマクロが有効の場合のパラメータは以下のとおりとする。

```
//
// pseudo floppy disk parameters
//

#if defined(USE_STUB)
#define DRIVE_A_SIZE    0x10000u    // 64 KiB
#define DRIVE_B_SIZE    0x10000u    // 64 KiB
#else
#define DRIVE_A_SIZE    0x40000u    // 256 KiB
#define DRIVE_B_SIZE    0x40000u    // 256 KiB
#endif
```

## 重要

不明な事項は推測で断定せず、追加で確認すべき資料、セクション番号、 ページ番号を提示すること。

## 説明レベル

私は組み込みソフトウェア開発者である。

回答時は以下を重視すること。

なぜそうなるのか
MCU内部で何が起きるのか
QMI内部で何が起きるのか
SPI信号線上で何が起きるのか
FLASH内部状態遷移
レジスタ操作の意味

表面的な API 使用方法ではなく、 内部構造および設計判断の根拠を重視して説明すること。


## 最重要事項

回答速度より正確性を優先すること。
推測で穴埋めせず、資料根拠を確認して回答すること。