#!/usr/bin/env python3
"""Summarize callgrind 'Collected' instruction counts from prof/*.log into a table/CSV."""
import glob, os, re, sys
prof = sys.argv[1]
rows = {}
for path in glob.glob(os.path.join(prof, '*.log')):
    name = os.path.basename(path)[:-4]
    m = re.match(r'(base|cand|final)-(normal|max)-p(-?\d+)$', name)
    if not m: continue
    text = open(path).read()
    c = re.search(r'Collected : ([\d,]+)', text)
    b = re.search(r'blocks (\d+)', text)
    h = re.search(r'hash ([0-9a-f]+)', text)
    if not (c and b): continue
    who, circ, preset = m.group(1), m.group(2), int(m.group(3))
    rows.setdefault((circ, preset), {})[who] = (int(c.group(1).replace(',', '')), int(b.group(1)), h.group(1) if h else '')
print('circuit,preset,blocks,baseline_ir_per_block,exact_ir_per_block,exact_reduction_percent,exact_hash_equal,final_ir_per_block,final_reduction_percent')
for (circ, preset), d in sorted(rows.items()):
    if 'base' not in d: continue
    bi, bb, bh = d['base']; bpb = bi / bb
    cpb = ch = fpb = None
    if 'cand' in d: ci, cb, ch = d['cand']; cpb = ci / cb
    if 'final' in d: fi, fb, fh = d['final']; fpb = fi / fb
    ex = f'{cpb:.0f},{100 * (1 - cpb / bpb):.2f},{"yes" if bh == ch else "NO"}' if cpb else ',,'
    fn = f'{fpb:.0f},{100 * (1 - fpb / bpb):.2f}' if fpb else ','
    print(f'{circ},{preset},{bb},{bpb:.0f},{ex},{fn}')
