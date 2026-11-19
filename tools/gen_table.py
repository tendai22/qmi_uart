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
    pattern = re.compile(r'WAIT_UNTIL_L\s*\(', re.MULTILINE)

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
            raise RuntimeError(f"cannot parse WAIT_UNTIL_L at line {lineno}")

        label = parts[1].strip()
        label = re.sub(r'\s+', '', label)
        entries.append((lineno, label))
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
            f.write(f"    PTLBL_{label},\n")

        f.write("    PTLBL_DONE,\n")
        f.write("    PTLBL_COUNT\n")
        f.write("} pt_label_id_t;\n\n")

        f.write("typedef struct\n")
        f.write("{\n")
        f.write("    int line;\n")
        f.write("    pt_label_id_t id;\n")
        f.write("    const char *name;\n")
        f.write("} pt_label_t;\n\n")

        f.write("extern const pt_label_t pt_labels[];\n")
        f.write("extern const uint32_t pt_labels_count;\n\n")
        f.write("const char *pt_label(int line);\n")
        f.write("void pt_stats_enter(int line);\n")
        f.write("void pt_stats_leave(int line, uint64_t elapsed_us);\n")
        f.write("void pt_stats_dump(void);\n")

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
        f.write("static pt_stat_t stats[PTLBL_COUNT];\n\n")

        #
        # pt_labels[]
        #
        f.write("const pt_label_t pt_labels[] =\n{\n")
        f.write('    { 0, PTLBL_START, "START" },\n')

        for lineno, label in entries:
            f.write(f'    {{ {lineno}, PTLBL_{label}, "{label}" }},\n')

        f.write('    { -1, PTLBL_DONE, "DONE" },\n')
        f.write("};\n\n")
        f.write("const uint32_t pt_labels_count =\n")
        f.write("    sizeof(pt_labels)/sizeof(pt_labels[0]);\n\n")

        #
        # pt_label()
        #
        f.write("const char *pt_label(int line)\n")
        f.write("{\n")
        f.write("    switch(line)\n")
        f.write("    {\n")
        f.write('    case 0: return "START";\n')

        for lineno, label in entries:
            f.write(f'    case {lineno}: return "{label}";\n')

        f.write('    case -1: return "DONE";\n')
        f.write('    default: return "?";\n')
        f.write("    }\n")
        f.write("}\n\n")

        #
        # pt_line_to_id()
        #
        f.write("static pt_label_id_t pt_line_to_id(int line)\n")
        f.write("{\n")
        f.write("    switch(line)\n")
        f.write("    {\n")
        f.write("    case 0:\n")
        f.write("        return PTLBL_START;\n")

        for lineno, label in entries:
            f.write(f"    case {lineno}:\n")
            f.write(f"        return PTLBL_{label};\n")

        f.write("    case -1:\n")
        f.write("        return PTLBL_DONE;\n")
        f.write("    default:\n")
        f.write("        return PTLBL_COUNT;\n")
        f.write("    }\n")
        f.write("}\n\n")

        #
        # statistics
        #
        f.write("void pt_stats_enter(int line)\n")
        f.write("{\n")
        f.write("    pt_label_id_t id = pt_line_to_id(line);\n")
        f.write("    if(id >= PTLBL_COUNT)\n")
        f.write("        return;\n")
        f.write("    stats[id].count++;\n")
        f.write("}\n\n")
        f.write("void pt_stats_leave(int line, uint64_t elapsed_us)\n")
        f.write("{\n")
        f.write("    pt_label_id_t id = pt_line_to_id(line);\n")
        f.write("    if(id >= PTLBL_COUNT)\n")
        f.write("        return;\n")
        f.write("    stats[id].total_us += elapsed_us;\n")
        f.write("}\n\n")
        f.write("void pt_stats_dump(void)\n")
        f.write("{\n")
        f.write("    for(uint32_t i=0; i<pt_labels_count; i++)\n")
        f.write("    {\n")
        f.write("        const pt_label_t *p = &pt_labels[i];\n")
        f.write("        pt_stat_t *s = &stats[p->id];\n")
        f.write('        debug("%-20s count=%lu total=%llu us",\n')
        f.write("              p->name,\n")
        f.write("              (unsigned long)s->count,\n")
        f.write("              s->total_us);\n")
        f.write("    }\n")
        f.write("}\n")

def main():
    if len(sys.argv) != 4:
        print("usage: gen_table.py foo.c foo_table.h foo_table.c")
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
