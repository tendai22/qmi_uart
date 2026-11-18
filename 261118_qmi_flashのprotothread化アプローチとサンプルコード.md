# qmi_flashのprotothread化、アプローチとサンプルコード

4kキャッシュロジックを逐次的に書くために、protothreadを導入する。
その前に、qmi_flash自体をprotothread化する。

## protothread

switch構文を使ったコルーチン的記述、逐次的な処理で「長い待ち」が入りそうなところで、
その場所を state 変数に保持し、次の switch 文をその state変数の状態から継続する。

マクロを使用して switch-case 構文を見えないようにして、見た目を逐次処理の流れで記述する。
for/whileループ途中で「長い待ち」が入っても適用できるので、分岐・ループも含めて見た目が自然になる。

協調的マルチタスク(内部でどこかのスレッドが抱え込まない)を実現する際に各タスクのコードを同期式に見えるような記述ができる。

例えば qmi_flash が poll 化できている場合、それを使ってキャッシュハンドラを書くと、

```
void CacheManager::poll()
{
    switch(line)
    {
    case 0:

        while(true)
        {
            op = flash_read_async(addr, pagebuf);

            WAIT_UNTIL(
                flash_op_done(op));

            modify_page(pagebuf);

            op = flash_erase4k_async(addr);

            WAIT_UNTIL(
                flash_op_done(op));

            op = flash_program_async(
                addr,
                pagebuf);

            WAIT_UNTIL(
                flash_op_done(op));
        }
    }
}
```

これ自体も poll なので(最上位の無限ループで poll が呼び出される)、flash_op_done()関数を用意すると
上記の記述(逐次的な、かつ whileループを使った記述)で書ける。

ここで、WAIT_UNTILは、

```
#define PT_WAIT_UNTIL(cond)                  \
    do {                                     \
        line = __LINE__;                     \
        case __LINE__:                       \
        if (!(cond))                         \
            return false;                    \
    } while (0)
```

である。condが成り立つまでは、次の行に行かずに return を続ける。2回目以後も line が保存されるので cond 結果のチェックが行われる。
condが成り立つと、このマクロ全体を抜けて、次の行に進む。

> flash 動作自身は、別途、最上位の無限ループで qmi_flash_poll(); として別途呼び出されるはずだ。

## flash_op

```
struct FlashOp {
    bool busy;
    bool completed;
    bool success;
};
```

に状態を保持しておき、

```
bool flash_op_done(FlashOp* op);
bool flash_op_success(FlashOp* op);
```

だけを上位に見せる。read_start/read_poll を触らなくてもよくなる。

## cache_managerをイベント駆動で作る

外部からの呼び出し

```
cache.request_read(lba);
cache.request_flush();
```

```
cache.poll();
```

を毎回呼び出す(Core1 main loop)

cache.poll() の中は

```
while(true)
{
    Request req = co_wait_request();

    Page* page =
        co_await load_page(req.lba);

    process(page);

    if(page->dirty)
        co_await flush_page(page);
}
```

的に書く。WAIT_UNTILを使うと、

```
BEGIN();

while(true)
{
    WAIT_UNTIL(has_request());

    page = find_page(req.lba);

    if(page == nullptr) {
        flash.read_start(...);
        WAIT_UNTIL(!flash.busy());
        load_cache();
    }

    if(page->dirty) {
        flash.erase_start(...);
        WAIT_UNTIL(!flash.busy());
        flash.program_start(...);
        WAIT_UNTIL(!flash.busy());
        page->dirty = false;
    }
}

END();
```

という感じ。こうすることで、Core1メインループが、

```
for(;;) {
    tud_task();
    flash.poll();
    cache.poll();
}
```

## いよいろ qmi_flash を protothread 化する

qmi_flash_erase の中で、

```
ST_ERASE_CMD
    ↓
ST_ERASE_WAIT_WEL
    ↓
ST_ERASE_SEND
    ↓
ST_ERASE_WAIT_BUSY
    ↓
DONE
```

となっているところで、

```
flash_write_enable();

WAIT_UNTIL(status_reg() & SR_WEL);

flash_send_erase4k(addr);

while(status_reg() & SR_BUSY)
{
    YIELD();
}
```

と書く感じ。じっさいには以下の手順がよいのではないか？

### STEP1: qmi_flash_read_status()

ステータスレジスタを読み出すだけの機能。

```
PT_BEGIN();
qmi_tx_start(CMD_READ_STATUS);
WAIT_UNTIL(!qmi_busy());
qmi_rx_start(1);
WAIT_UNTIL(!qmi_busy());
status = rxbuf[0];
PT_END();
```

### STEP2: qmi_erase_4k()

```
PT_BEGIN();
write_enable();
WAIT_UNTIL(done());
send_erase(addr);
WAIT_UNTIL(done());
do {
    read_status();
    WAIT_UNTIL(done());
} while(status & SR_BUSY);

PT_END();
```

まず、

```
flash_read(addr, buf, len);
flash_erase4k(addr);
flash_program_page(addr, buf);
```

を同期版で書き下ろして、どこに WAIT_QMI(), WAIT_BUSY_CLEAR(); を差し込んで protothread化する。

### STEP1: protothread 基盤だけ作る

```
#define PT_BEGIN() \
    switch(pt_line) { \
    case 0:

#define PT_END() \
    } \
    pt_line = 0; \
    return true

#define PT_WAIT_UNTIL(cond) \
    do { \
        pt_line = __LINE__; \
        case __LINE__: \
        if (!(cond)) return false; \
    } while(0)
```

### STEP2: read_sr1 (status register read)を書く

```
bool ReadStatusTask::poll()
{
    PT_BEGIN();

    qmi_send_cmd(CMD_RDSR);

    PT_WAIT_UNTIL(qmi_tx_complete());

    qmi_recv_start(1);

    PT_WAIT_UNTIL(qmi_rx_complete());

    status = rxbuf[0];

    PT_END();
}
```

### STEP3: erase_4k

同期版

```
    write_enable();

    issue_sector_erase(addr);

    while (busy())
    {
    }
```

を

```
PT_BEGIN();

write_enable_start();

PT_WAIT_UNTIL(write_enable_done());

erase_start(addr);

PT_WAIT_UNTIL(erase_cmd_sent());

while (true)
{
    read_sr1_start();

    PT_WAIT_UNTIL(read_sr1_done());

    if (!(sr1 & SR_BUSY))
        break;
}

PT_END();
```

### STEP4: program_page

```
for(offset=0; offset<256; offset+=8) {
    qmi_write8(...);

    PT_WAIT_UNTIL(
        qmi_tx_complete());
}
```

の形にできる。

### 注意点

実際の C言語コードの際には、

* ローカル変数は使えない。タスクオブジェクトを用意し、そこに書く
  例えば、addr, offset を保持する構造体 ProgramTask を用意する、みたいな。

```
static ProgramTask {
    int line;   // 再開位置
    uint32_t offset;
};
```

出発点は、

```
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

> Adam Dunkels の元祖 Protothread のドキュメントにも注意があります。

だそうなので、それを調べておく。

```
write_enable();

send_program_command();

for(offset=0; offset<256; offset+=8)
{
    send8(...);
}

wait_busy_clear();
```

が、

```
PT_BEGIN();

write_enable();
WAIT_FLASH();

send_program_command();
WAIT_FLASH();

for(offset=0; offset<256; offset+=8)
{
    send8(...);

    WAIT_FLASH();
}

wait_busy_clear();
WAIT_BUSY_CLEAR();

PT_END();
```

となる感じ。主要プリミティブごとに WAIT_...()が入る。

 　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　