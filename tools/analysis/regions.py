#!/usr/bin/env python3
"""Summarise analysis/functions.tsv per region of config/regions.txt.
Usage: python -I tools/analysis/regions.py config/regions.txt analysis/functions.tsv
"""
import sys

def load_regions(path):
    regs = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        a, b, kind, conf, label = line.split(None, 4)
        regs.append((int(a, 16), int(b, 16), kind, conf, label))
    return regs

def main(reg_path, fn_path):
    regs = load_regions(reg_path)
    fns = [l.split('\t') for l in open(fn_path).read().splitlines()[1:]]
    fns = [(int(f[0], 16), int(f[1])) for f in fns]
    print('%-19s %-4s %-4s %8s %6s  %s' % ('range', 'kind', 'conf', 'KB', 'funcs', 'label'))
    totals = {}
    for a, b, kind, conf, label in regs:
        n = sum(1 for f, _ in fns if a <= f < b) if kind == 'ee' else 0
        kb = (b - a) / 1024
        totals[kind] = totals.get(kind, 0) + kb
        print('%08x-%08x %-4s %-4s %8.1f %6s  %s' % (a, b, kind, conf, kb, n or '', label[:70]))
    print('totals: ' + ', '.join('%s %.0f KB' % kv for kv in sorted(totals.items())))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
