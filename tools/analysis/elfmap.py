#!/usr/bin/env python3
"""M0 analysis: discover functions in SLUS_208.87 and attach string references.

Writes analysis/functions.tsv  (addr, size, callers, strings referenced)
Usage: python -I tools/analysis/elfmap.py extracted/disc/SLUS_208.87 analysis [text_end_hex]
"""
import os, re, struct, sys
from collections import defaultdict

RA_RET = 0x03E00008          # jr $ra


def load_elf(path):
    e = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', e, 0x1C)
    phnum, = struct.unpack_from('<H', e, 0x2C)
    segs = []
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from('<8I', e, phoff + i * 32)
        if p_type == 1 and filesz:
            segs.append((vaddr, e[off:off + filesz]))
    entry, = struct.unpack_from('<I', e, 0x18)
    return entry, segs


class Image:
    def __init__(self, base, data):
        self.base, self.data = base, data
        self.end = base + len(data)

    def word(self, a):
        return struct.unpack_from('<I', self.data, a - self.base)[0]

    def contains(self, a):
        return self.base <= a < self.end

    def cstring(self, a):
        if not self.contains(a):
            return None
        o = a - self.base
        z = self.data.find(b'\0', o, o + 512)
        if z <= o:
            return None
        s = self.data[o:z]
        if len(s) < 3 or any(c < 0x20 and c not in (9, 10, 13) or c > 0x7e for c in s):
            return None
        return s.decode('ascii')


def find_code_end(img, start):
    """Code ends where 64 KB blocks stop containing returns."""
    blk = 0x10000
    a = start
    last = start
    while a < img.end:
        n = min(blk, img.end - a) // 4
        words = struct.unpack_from('<%dI' % n, img.data, a - img.base)
        if words.count(RA_RET) > 20:
            last = a + n * 4
        elif last != start and a - last > 2 * blk:
            break
        a += blk
    # trim back to the last jr $ra + delay slot
    a = last - 4
    while a > start and img.word(a) != RA_RET:
        a -= 4
    return a + 8


def discover(img, text_lo, text_hi, entry):
    starts = {entry}
    callers = defaultdict(set)
    words = struct.unpack_from('<%dI' % ((text_hi - text_lo) // 4), img.data, text_lo - img.base)
    for i, w in enumerate(words):
        pc = text_lo + i * 4
        if w >> 26 == 3:                                    # jal
            t = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if text_lo <= t < text_hi:
                starts.add(t)
                callers[t].add(pc)
    # function after a return whose first insn allocates stack
    for i, w in enumerate(words[:-2]):
        if w == RA_RET:
            j = i + 2
            while j < len(words) and words[j] == 0:        # alignment padding
                j += 1
            if j < len(words) and words[j] >> 16 == 0x27BD and words[j] & 0x8000:
                starts.add(text_lo + j * 4)
    # function pointers in data (vtables, callback tables)
    data_ptrs = defaultdict(int)
    for o in range(text_hi - img.base, len(img.data) - 3, 4):
        v, = struct.unpack_from('<I', img.data, o)
        if text_lo <= v < text_hi and v & 3 == 0:
            data_ptrs[v] += 1
    for v in data_ptrs:
        prev = img.word(v - 4) if v - 4 >= text_lo else 0
        prev2 = img.word(v - 8) if v - 8 >= text_lo else 0
        if prev2 == RA_RET or prev == 0 or img.word(v) >> 16 == 0x27BD:
            starts.add(v)
    return sorted(starts), callers, data_ptrs


def string_refs(img, lo, hi):
    """Track lui hi-halves per register; resolve addiu/ori/lw/sw-style lo halves."""
    refs = set()
    hi_regs = {}
    for pc in range(lo, hi, 4):
        w = img.word(pc)
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 0x0F:                                       # lui
            hi_regs[rt] = imm << 16
            continue
        if rs in hi_regs and op in (0x09, 0x0D):              # addiu / ori
            v = hi_regs[rs] + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)
            v &= 0xFFFFFFFF
            s = img.cstring(v)
            if s:
                refs.add((v, s))
        if op == 0x02 or op == 0x03:                          # j/jal: lose nothing
            continue
    return refs


def main(elf_path, out_dir):
    entry, segs = load_elf(elf_path)
    img = Image(*segs[0])
    text_lo = img.base
    text_hi = find_code_end(img, text_lo)
    if len(sys.argv) > 3:   # optional explicit end (start of VU microcode)
        text_hi = int(sys.argv[3], 16)
    starts, callers, data_ptrs = discover(img, text_lo, text_hi, entry)
    os.makedirs(out_dir, exist_ok=True)
    rows = []
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else text_hi
        refs = sorted(string_refs(img, s, e))
        rows.append((s, e - s, len(callers[s]), data_ptrs.get(s, 0), refs))
    with open(os.path.join(out_dir, 'functions.tsv'), 'w') as o:
        o.write('addr\tsize\tcallers\tdata_refs\tstrings\n')
        for s, size, nc, nd, refs in rows:
            o.write('%08x\t%d\t%d\t%d\t%s\n' % (s, size, nc, nd,
                    ' | '.join(r[1].encode('unicode_escape').decode('ascii') for r in refs)))
    print('text %08x-%08x (%d KB), %d functions, %d with string refs'
          % (text_lo, text_hi, (text_hi - text_lo) // 1024, len(rows), sum(1 for r in rows if r[4])))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
