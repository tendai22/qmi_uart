# qmi_floppy/flash関連エラーコードの扱い方針

pt_qmi_flash.c/その上位の pt_qmi_floppy.cでのエラーコード

## 3階層からなる

最上位: pt_qmi_floppyコマンド(非同期 read/write sector)
  pt_issue_floppy_read(int drive, uint32_t lba, uint8_t *buffer);
  pt_issue_floppy_write(int drive, uint32_t lba, uint8_t *buffer);
  void pt_qmi_floppy_poll(void);

* pt_qmi_flash層のコマンドを使用して、仮想フロッピーセクタリードライト機能を実現する。

中位:   pt_qmi_flashコマンド(非同期 read/erase/program)
  bool pt_issue_erase(uint32_t address);
  bool pt_issue_program(uint32_t address, uint8_t *buffer, uint32_t length);
  bool pt_issue_read(uint32_t address, uint8_t *buffer, uint32_t length);
  void pt_qmi_flash_poll(void);

* 内蔵フラッシュの read/erase_4k/program_page を実行する。レジスタアクセスは下位層 qmi_flash.c 関数群を用いる。

下位:   qmi_flash.c(同期QMIレジスタアクセスプリミティブ)
  bool qmi_wait_not_busy();     // false: timeout, true: OK
  bool qmi_wait_tx_ready(void); // false: timeout, true: OK
  bool qmi_wait_rx_ready(void); // false: timeout, true: OK
  bool qmi_direct_mode_start(void); // false: timeout, true: OK
  bool qmi_direct_mode_stop(void);  // false: timeout, true: OK
  bool qmi_flash_read_register(uint8_t command, uint8_t *buffer, size_t length); // false: param error, timeout, true: ok
  bool qmi_wait_busy_clear(void);   // false: timeout, true: OK
  bool qmi_flash_read_data(uint32_t address, uint8_t *buffer, size_t length);
  bool qmi_flash_write_byte(uint8_t data);  // false: timeout, true: ok
  bool qmi_flash_write_enable(void); // 
  bool qmi_flash_status_ready(bool *ready); // 

## エラーハンドリング

下位層qmi_flash.c関数群は、static global変数 qmi_lower_error にエラー発生を格納する。と同時に返し値で false を返す。

下位層エラーコードは 関数 int qmi_error(void) で取り出すことができる。中位関数が下位層関数を呼び出し false で返った時は、 qmi_error()を呼び出し、そのエラーコードを自身のエラー変数 pt->error に保持しておく。

中位層関数内部でのエラー発生時は pt->error に保持する。

下位層、中位層のエラーコードは共通で、pt_qmi.h /qmi_flash.h ともに共通のエラーコード定義ヘッダ qmi_error.h を #include することで、エラーコードに齟齬がないようにしている。

bool qmi_flash_status_ready(bool *ready); は呼び出しシーケンスも変更して 引数なしとする。リターン時 true の場合、正常終了(デバイスSR1 BUSY解除==デバイスの処理完了) またはQMIデバイスアクセスがタイムアウトということなので、true で返ってきた後はかならず qmi_error(); を呼び出してタイムアウトかどうかを確認する。


