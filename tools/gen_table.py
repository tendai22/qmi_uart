#!/usr/bin/env python3
"""Generate source-specific Protothread labels and poll statistics.
Usage: gen_table.py foo.c foo_table.h foo_table.c
"""
import re
import sys
from pathlib import Path


def parse_wait_labels(source):
    entries = []
    # Macro calls must start on a single source line, as agreed.
    for lineno, line in enumerate(source.splitlines(), 1):
        match = re.search(r'\bPT_WAIT_UNTIL_L\s*\(.*?,\s*([A-Za-z_]\w*)\s*\)\s*;', line)
        if match:
            entries.append((lineno, match.group(1)))
    if len({name for _, name in entries}) != len(entries):
        raise ValueError('Duplicate PT_WAIT_UNTIL_L label in source')
    return entries


def generate(src, hdr, csrc):
    entries = parse_wait_labels(src.read_text(encoding='utf-8'))
    prefix = src.stem
    if not re.fullmatch(r'[A-Za-z_]\w*', prefix):
        raise ValueError('Invalid source basename')
    upper = prefix.upper()
    enum = f'{prefix}_label_id_t'
    struct = f'{prefix}_label_t'
    labels = f'{prefix}_labels'
    count = f'{prefix}_labels_count'
    lookup = f'{prefix}_label'
    enter = f'{prefix}_stats_enter'
    leave = f'{prefix}_stats_leave'
    dump = f'{prefix}_stats_dump'
    head = ['#pragma once', '#include <stdint.h>',
            f'typedef enum {{ {upper}_LBL_START,']
    head += [f'    {upper}_LBL_{label},' for _, label in entries]
    head += [f'    {upper}_LBL_DONE, {upper}_LBL_COUNT', f'}} {enum};',
             f'typedef struct {{ int line; {enum} id; const char *name; }} {struct};',
             f'extern const {struct} {labels}[];',
             f'extern const uint32_t {count};',
             f'const char *{lookup}(int line);',
             f'void {enter}(int line);',
             f'void {leave}(int line, uint64_t elapsed_us);',
             f'void {dump}(void);']
    hdr.write_text('\n'.join(head) + '\n', encoding='utf-8')
    rows = [(0, 'START')] + entries + [(-1, 'DONE')]
    idof = lambda name: f'{upper}_LBL_{name}'
    body = [f'#include "{hdr.name}"', '#include "memprt.h"',
            'typedef struct { uint32_t count; uint64_t total_us; } pt_stat_t;',
            f'static pt_stat_t stats[{upper}_LBL_COUNT];',
            f'const {struct} {labels}[] = {{']
    body += [f'    {{ {line}, {idof(name)}, "{name}" }},' for line, name in rows]
    body += ['};', f'const uint32_t {count} = sizeof({labels})/sizeof({labels}[0]);',
             f'const char *{lookup}(int line) {{ switch(line) {{']
    body += [f'case {line}: return "{name}";' for line, name in rows]
    body += ['default: return "?"; } }',
             f'static {enum} line_to_id(int line) {{ switch(line) {{']
    body += [f'case {line}: return {idof(name)};' for line, name in rows]
    body += [f'default: return {upper}_LBL_COUNT; }} }}',
             f'void {enter}(int line) {{ {enum} id = line_to_id(line);',
             f'if (id < {upper}_LBL_COUNT) stats[id].count++; }}',
             f'void {leave}(int line, uint64_t us) {{ {enum} id = line_to_id(line);',
             f'if (id < {upper}_LBL_COUNT) stats[id].total_us += us; }}',
             f'void {dump}(void) {{ for (uint32_t i=0; i<{count}; ++i) {{',
             f'const {struct} *p = &{labels}[i];',
             'pt_stat_t *s = &stats[p->id];',
             'debug("%-28s count=%lu total=%llu us\\n", p->name,',
             '(unsigned long)s->count, (unsigned long long)s->total_us);',
             '} }']
    csrc.write_text('\n'.join(body) + '\n', encoding='utf-8')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit('usage: gen_table.py foo.c foo_table.h foo_table.c')
    generate(*(Path(x) for x in sys.argv[1:]))
