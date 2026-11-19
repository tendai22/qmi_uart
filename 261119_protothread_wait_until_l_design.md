# Protothread + WAIT_UNTIL_L + 自動ラベル生成 + CMakeLists.txt にプリプロセス処理を組み込む

## 概要

通常の Protothread では、

```c
WAIT_UNTIL(erase_done());
```

の待ち位置は実行時に見えなくなる。

そこで

```c
WAIT_UNTIL_L(erase_done(), ERASE_DONE);
```

と書き、

ソースコードから自動的に

- 行番号 ↔ ラベル名変換表
- enum識別子
- 統計カウンタ

を生成する。

---

# foo.c

## protothread本体

```c
#include "protothread.h"
#include "foo_table.h"

struct flash_pt
{
    int line;
    uint32_t addr;
};

void flash_poll(struct flash_pt *pt)
{
    PT_BEGIN(pt);

    flash_read_start(pt->addr);

    WAIT_UNTIL_L(
        flash_read_done(),
        READ_DONE
    );

    modify_buffer();

    flash_erase_start(pt->addr);

    WAIT_UNTIL_L(
        flash_erase_done(),
        ERASE_DONE
    );

    flash_program_start(pt->addr);

    WAIT_UNTIL_L(
        flash_program_done(),
        PROGRAM_DONE
    );

    PT_END(pt);
}
```

---

# gen_table.py が検出するもの

```c
WAIT_UNTIL_L(
    flash_read_done(),
    READ_DONE
);

WAIT_UNTIL_L(
    flash_erase_done(),
    ERASE_DONE
);

WAIT_UNTIL_L(
    flash_program_done(),
    PROGRAM_DONE
);
```

から

```text
READ_DONE
ERASE_DONE
PROGRAM_DONE
```

を抽出する。

---

# 生成結果: foo_table.h

```c
#pragma once

#include <stdint.h>

typedef enum
{
    PTLBL_READ_DONE,
    PTLBL_ERASE_DONE,
    PTLBL_PROGRAM_DONE,

    PTLBL_COUNT
} pt_label_id_t;

typedef struct
{
    int line;
    pt_label_id_t id;
    const char *name;
} pt_label_t;

const char *pt_label(int line);

extern const pt_label_t pt_labels[];
extern const uint32_t pt_labels_count;

void pt_stats_enter(int line);
void pt_stats_leave(int line, uint64_t elapsed_us);
void pt_stats_dump(void);
```

---

# 生成結果: foo_table.c

```c
#include "foo_table.h"
#include "debug.h"

typedef struct
{
    uint32_t count;
    uint64_t total_us;
} pt_stat_t;

static pt_stat_t stats[PTLBL_COUNT];
```

## ラベルテーブル

```c
const pt_label_t pt_labels[] =
{
    { 32, PTLBL_READ_DONE,    "READ_DONE"    },
    { 41, PTLBL_ERASE_DONE,   "ERASE_DONE"   },
    { 49, PTLBL_PROGRAM_DONE, "PROGRAM_DONE" }
};

const uint32_t pt_labels_count =
    sizeof(pt_labels)/sizeof(pt_labels[0]);
```

## line → label変換

```c
const char *pt_label(int line)
{
    switch(line)
    {
    case 32:
        return "READ_DONE";

    case 41:
        return "ERASE_DONE";

    case 49:
        return "PROGRAM_DONE";

    default:
        return "?";
    }
}
```

## line → enum変換

```c
static pt_label_id_t pt_line_to_id(int line)
{
    switch(line)
    {
    case 32:
        return PTLBL_READ_DONE;

    case 41:
        return PTLBL_ERASE_DONE;

    case 49:
        return PTLBL_PROGRAM_DONE;

    default:
        return PTLBL_COUNT;
    }
}
```

## 統計開始

```c
void pt_stats_enter(int line)
{
    pt_label_id_t id =
        pt_line_to_id(line);

    if(id >= PTLBL_COUNT)
        return;

    stats[id].count++;
}
```

## 統計終了

```c
void pt_stats_leave(
    int line,
    uint64_t elapsed_us)
{
    pt_label_id_t id =
        pt_line_to_id(line);

    if(id >= PTLBL_COUNT)
        return;

    stats[id].total_us += elapsed_us;
}
```

## 統計表示

```c
void pt_stats_dump(void)
{
    for(uint32_t i=0;
        i < pt_labels_count;
        i++)
    {
        const pt_label_t *p =
            &pt_labels[i];

        pt_stat_t *s =
            &stats[p->id];

        debug(
            "%-20s count=%lu total=%llu us",
            p->name,
            (unsigned long)s->count,
            s->total_us
        );
    }
}
```

---

# protothread.h追加コード

## ラベル付きWAIT

```c
#define WAIT_UNTIL_L(cond, label) \
    do { \
        pt->line = __LINE__; \
        if(!(cond)) \
            return; \
        case __LINE__:; \
    } while(0)
```

## PT_BEGIN

```c
#define PT_BEGIN(pt) \
    switch((pt)->line) \
    { \
    case 0:
```

## PT_END

```c
#define PT_END(pt) \
    } \
    (pt)->line = 0; \
    return
```

---

# 計測ラッパ

```c
uint64_t start;
uint64_t elapsed;

int old_line;

old_line = pt.line;

start = time_us_64();

flash_poll(&pt);

elapsed = time_us_64() - start;

pt_stats_enter(old_line);
pt_stats_leave(old_line, elapsed);
```

---

# 状態遷移ログ

```c
int old_line = pt.line;

flash_poll(&pt);

if(old_line != pt.line)
{
    debug(
        "%s -> %s",
        pt_label(old_line),
        pt_label(pt.line)
    );
}
```

出力例:

```text
READ_DONE -> ERASE_DONE

ERASE_DONE -> PROGRAM_DONE

PROGRAM_DONE -> ?
```

---

# 改良版（おすすめ）

実務では `?` を避けるため、

```c
PT_BEGIN
```

に対応する

```c
START
```

と

```c
PT_END
```

に対応する

```c
DONE
```

を予約ラベルとして持たせる。

```text
START -> READ_DONE

READ_DONE -> ERASE_DONE

ERASE_DONE -> PROGRAM_DONE

PROGRAM_DONE -> DONE
```

となり、シリアルログを見るだけで Protothread の実行経路が完全に追跡できる。

---

# 最終イメージ

ソース側は単に

```c
WAIT_UNTIL_L(flash_read_done(), READ_DONE);
WAIT_UNTIL_L(flash_erase_done(), ERASE_DONE);
WAIT_UNTIL_L(flash_program_done(), PROGRAM_DONE);
```

と書くだけ。

生成器が

- enum生成
- line→name変換
- line→id変換
- 統計用配列
- 統計表示関数
- ラベル一覧

を生成する。

その結果、

```text
READ_DONE          count=1      total=52 us
ERASE_DONE         count=131    total=13485342 us
PROGRAM_DONE       count=131    total=79521 us
```

のようなプロファイルが簡単に取得でき、Protothread が「どこで時間を使っているか」をほぼ自動で可視化できる。これは RP2040/RP2350 上のポーリングベース設計と非常に相性が良いと思います。

# pico-sdk で CMakeLists.txt に追記する内容

ポイントは、

* find_package(Python3 REQUIRED COMPONENTS Interpreter)
* ${Python3_EXECUTABLE}
* ${CMAKE_CURRENT_BINARY_DIR} に生成
* target_sources() で生成物を登録

です。

```
project/
├── CMakeLists.txt
├── foo.c
├── main.c
└── tools/
    └── gen_table.py
```

生成後

```
build/
├── foo_table.c
└── foo_table.h
```

## CMakeLists.txt

```
cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(my_app C CXX ASM)

pico_sdk_init()

find_package(
    Python3 REQUIRED
    COMPONENTS Interpreter
)

#
# source
#

set(FOO_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/foo.c
)

#
# generated
#

set(FOO_TABLE_H
    ${CMAKE_CURRENT_BINARY_DIR}/foo_table.h
)

set(FOO_TABLE_C
    ${CMAKE_CURRENT_BINARY_DIR}/foo_table.c
)

#
# foo.c
#   ↓
# gen_table.py
#   ↓
# foo_table.[ch]
#

add_custom_command(

    OUTPUT
        ${FOO_TABLE_H}
        ${FOO_TABLE_C}

    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_table.py
        ${FOO_SRC}
        ${FOO_TABLE_H}
        ${FOO_TABLE_C}

    DEPENDS
        ${FOO_SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_table.py

    COMMENT
        "Generating foo_table.[ch]"

    VERBATIM
)

```

## 汎用ルール化

```
set(GEN_TABLE_SRC_FILES
    cache_manager.c
    flash_driver.c
    cdc_tx.c
)
```

とかくだけで、一括生成してくれるCMakeLists.txt
Linux, Windows, Ninja, Visual Studio, Raspberry Pi Pico SDK
すべてで共通に書ける。

### CMakeLists.txt 完全版

```cmake
cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(
    my_app
    C
    CXX
    ASM
)

pico_sdk_init()

#
# Python
#
# Linux:
#   /usr/bin/python3
#
# Windows:
#   python.exe
#
# が自動選択される
#

find_package(
    Python3
    REQUIRED
    COMPONENTS Interpreter
)

#
# gen_table.py
#

set(
    GEN_TABLE_PY
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_table.py
)

#
# Protothread対象ファイル
#

set(
    GEN_TABLE_SRC_FILES

    cache_manager.c
    flash_driver.c
    cdc_tx.c
)

#
# 生成物一覧
#

set(GENERATED_TABLE_SRCS)
set(GENERATED_TABLE_HDRS)

#
# cache_manager.c
#      ↓
# cache_manager_table.[ch]
#
# flash_driver.c
#      ↓
# flash_driver_table.[ch]
#
# ...
#

foreach(SRC ${GEN_TABLE_SRC_FILES})
    #
    # cache_manager
    #
    get_filename_component(BASE ${SRC} NAME_WE)

    #
    # source path
    #
    set(SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${SRC})

    #
    # generated paths
    #
    set(GENERATED_H ${CMAKE_CURRENT_BINARY_DIR}/${BASE}_table.h)
    set(GENERATED_C ${CMAKE_CURRENT_BINARY_DIR}/${BASE}_table.c)

    #
    # foo.c
    #    ↓
    # gen_table.py
    #    ↓
    # foo_table.h
    # foo_table.c
    #

    add_custom_command(
        OUTPUT
            ${GENERATED_H}
            ${GENERATED_C}
        COMMAND
            ${Python3_EXECUTABLE}
            ${GEN_TABLE_PY}
            ${SOURCE_FILE}
            ${GENERATED_H}
            ${GENERATED_C}
        DEPENDS
            ${SOURCE_FILE}
            ${GEN_TABLE_PY}
        COMMENT
            "Generating ${BASE}_table.[ch]"
        VERBATIM
    )

    list(
        APPEND
        GENERATED_TABLE_SRCS
        ${GENERATED_C}
    )

    list(
        APPEND
        GENERATED_TABLE_HDRS
        ${GENERATED_H}
    )
endforeach()

#
# 生成専用ターゲット
#

add_custom_target(generate_pt_tables
    DEPENDS
        ${GENERATED_TABLE_SRCS}
        ${GENERATED_TABLE_HDRS}
)

#
# アプリ
#

add_executable(my_app
    main.c
    ${GEN_TABLE_SRC_FILES}
    ${GENERATED_TABLE_SRCS}
)

#
# build/foo_table.h
# を #include できるようにする
#

target_include_directories(my_app
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)

#
# 先に生成
#

add_dependencies(my_app
    generate_pt_tables
)

#
# Pico SDK
#

target_link_libraries(my_app
    pico_stdlib
)

pico_add_extra_outputs(
    my_app
)
```

動作確認後、

```
function(add_pt_sources)
    foreach(SRC ${ARGV})
        ...
    endforeach()
endfunction()
```

として関数 add_pt_sources を定義し、

```
add_pt_sources(
    cache_manager.c
    flash_driver.c
    cdc_tx.c
)
```

と書くだけで追加処理が自動で走るようになる。

# 参考: gen_table.py

前提として、この gen_table.py は

```C
WAIT_UNTIL_L(flash_read_done(), READ_DONE);
WAIT_UNTIL_L(flash_erase_done(), ERASE_DONE);
WAIT_UNTIL_L(flash_program_done(), PROGRAM_DONE);
```

を検出して、

```
foo_table.h
foo_table.c
```

を生成します。

また、

```c
WAIT_UNTIL_L(
    flash_erase_done(),
    ERASE_DONE
);
```

のような複数行記述にも対応しています。

```py
#!/usr/bin/env python3

#
# gen_table.py
#
# usage:
#
#   python gen_table.py foo.c foo_table.h foo_table.c
#
# source:
#
#   WAIT_UNTIL_L(cond, LABEL)
#
# generated:
#
#   enum pt_label_id_t
#   pt_labels[]
#   pt_label()
#   pt_line_to_id()
#

import re
import sys
from pathlib import Path


def parse_wait_labels(source_text):
    """
    WAIT_UNTIL_L(..., LABEL)
    の LABEL と行番号を抽出する。
    """

    entries = []

    pattern = re.compile(
        r'WAIT_UNTIL_L\s*\(',
        re.MULTILINE
    )

    pos = 0

    while True:

        m = pattern.search(source_text, pos)

        if not m:
            break

        start = m.start()

        depth = 1
        i = m.end()

        while i < len(source_text):

            ch = source_text[i]

            if ch == '(':
                depth += 1

            elif ch == ')':
                depth -= 1

                if depth == 0:
                    break

            i += 1

        block = source_text[m.end():i]

        lineno = source_text.count('\n', 0, start) + 1

        parts = block.rsplit(',', 1)

        if len(parts) != 2:
            raise RuntimeError(
                f"cannot parse WAIT_UNTIL_L at line {lineno}"
            )

        label = parts[1].strip()

        label = re.sub(r'\s+', '', label)

        entries.append(
            (lineno, label)
        )

        pos = i

    return entries


def write_header(outfile, entries):

    labels = [x[1] for x in entries]

    with open(outfile, "w", encoding="utf-8") as f:

        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n\n")

        f.write("typedef enum\n")
        f.write("{\n")

        f.write("    PTLBL_START,\n")

        for label in labels:
            f.write(
                f"    PTLBL_{label},\n"
            )

        f.write("    PTLBL_DONE,\n")
        f.write("    PTLBL_COUNT\n")
        f.write("} pt_label_id_t;\n\n")

        f.write("typedef struct\n")
        f.write("{\n")
        f.write("    int line;\n")
        f.write("    pt_label_id_t id;\n")
        f.write("    const char *name;\n")
        f.write("} pt_label_t;\n\n")

        f.write(
            "extern const pt_label_t pt_labels[];\n"
        )

        f.write(
            "extern const uint32_t pt_labels_count;\n\n"
        )

        f.write(
            "const char *pt_label(int line);\n"
        )

        f.write(
            "void pt_stats_enter(int line);\n"
        )

        f.write(
            "void pt_stats_leave(int line, uint64_t elapsed_us);\n"
        )

        f.write(
            "void pt_stats_dump(void);\n"
        )


def write_source(outfile, entries):

    labels = [x[1] for x in entries]

    with open(outfile, "w", encoding="utf-8") as f:

        f.write('#include "foo_table.h"\n')
        f.write('#include "debug.h"\n\n')

        f.write("typedef struct\n")
        f.write("{\n")
        f.write("    uint32_t count;\n")
        f.write("    uint64_t total_us;\n")
        f.write("} pt_stat_t;\n\n")

        f.write(
            "static pt_stat_t stats[PTLBL_COUNT];\n\n"
        )

        #
        # pt_labels[]
        #

        f.write(
            "const pt_label_t pt_labels[] =\n{\n"
        )

        f.write(
            '    { 0, PTLBL_START, "START" },\n'
        )

        for lineno, label in entries:

            f.write(
                f'    {{ {lineno}, PTLBL_{label}, "{label}" }},\n'
            )

        f.write(
            '    { -1, PTLBL_DONE, "DONE" },\n'
        )

        f.write(
            "};\n\n"
        )

        f.write(
            "const uint32_t pt_labels_count =\n"
        )

        f.write(
            "    sizeof(pt_labels)/sizeof(pt_labels[0]);\n\n"
        )

        #
        # pt_label()
        #

        f.write(
            "const char *pt_label(int line)\n"
        )

        f.write("{\n")

        f.write(
            "    switch(line)\n"
        )

        f.write(
            "    {\n"
        )

        f.write(
            '    case 0: return "START";\n'
        )

        for lineno, label in entries:

            f.write(
                f'    case {lineno}: return "{label}";\n'
            )

        f.write(
            '    case -1: return "DONE";\n'
        )

        f.write(
            '    default: return "?";\n'
        )

        f.write(
            "    }\n"
        )

        f.write(
            "}\n\n"
        )

        #
        # pt_line_to_id()
        #

        f.write(
            "static pt_label_id_t pt_line_to_id(int line)\n"
        )

        f.write(
            "{\n"
        )

        f.write(
            "    switch(line)\n"
        )

        f.write(
            "    {\n"
        )

        f.write(
            "    case 0:\n"
        )

        f.write(
            "        return PTLBL_START;\n"
        )

        for lineno, label in entries:

            f.write(
                f"    case {lineno}:\n"
            )

            f.write(
                f"        return PTLBL_{label};\n"
            )

        f.write(
            "    case -1:\n"
        )

        f.write(
            "        return PTLBL_DONE;\n"
        )

        f.write(
            "    default:\n"
        )

        f.write(
            "        return PTLBL_COUNT;\n"
        )

        f.write(
            "    }\n"
        )

        f.write(
            "}\n\n"
        )

        #
        # statistics
        #

        f.write(
            "void pt_stats_enter(int line)\n"
        )

        f.write(
            "{\n"
        )

        f.write(
            "    pt_label_id_t id = pt_line_to_id(line);\n"
        )

        f.write(
            "    if(id >= PTLBL_COUNT)\n"
        )

        f.write(
            "        return;\n"
        )

        f.write(
            "    stats[id].count++;\n"
        )

        f.write(
            "}\n\n"
        )

        f.write(
            "void pt_stats_leave(int line, uint64_t elapsed_us)\n"
        )

        f.write(
            "{\n"
        )

        f.write(
            "    pt_label_id_t id = pt_line_to_id(line);\n"
        )

        f.write(
            "    if(id >= PTLBL_COUNT)\n"
        )

        f.write(
            "        return;\n"
        )

        f.write(
            "    stats[id].total_us += elapsed_us;\n"
        )

        f.write(
            "}\n\n"
        )

        f.write(
            "void pt_stats_dump(void)\n"
        )

        f.write(
            "{\n"
        )

        f.write(
            "    for(uint32_t i=0; i<pt_labels_count; i++)\n"
        )

        f.write(
            "    {\n"
        )

        f.write(
            "        const pt_label_t *p = &pt_labels[i];\n"
        )

        f.write(
            "        pt_stat_t *s = &stats[p->id];\n"
        )

        f.write(
            '        debug("%-20s count=%lu total=%llu us",\n'
        )

        f.write(
            "              p->name,\n"
        )

        f.write(
            "              (unsigned long)s->count,\n"
        )

        f.write(
            "              s->total_us);\n"
        )

        f.write(
            "    }\n"
        )

        f.write(
            "}\n"
        )


def main():

    if len(sys.argv) != 4:

        print(
            "usage: gen_table.py foo.c foo_table.h foo_table.c"
        )

        sys.exit(1)

    src = Path(sys.argv[1])
    hdr = Path(sys.argv[2])
    csrc = Path(sys.argv[3])

    text = src.read_text(encoding="utf-8")

    entries = parse_wait_labels(text)

    write_header(hdr, entries)

    write_source(csrc, entries)

    print("generated:")
    print(" ", hdr)
    print(" ", csrc)


if __name__ == "__main__":
    main()

```