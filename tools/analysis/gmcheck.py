#!/usr/bin/env python3
"""Validate every compiled GameMonkey library (.gm) against the GM 1.2x bytecode set.

Checks per instruction: opcode is known, branch targets land on instruction
boundaries, string/symbol operands point at the start of a string-table entry,
PUSHFN operands name an existing function.
Usage: python -I tools/analysis/gmcheck.py extracted/data [-v FILE]
"""
import os, struct, sys
from collections import Counter

OPS = ('GETDOT SETDOT GETIND SETIND ADD SUB MUL DIV REM BOR BXOR BAND SHL SHR BINV '
       'LT GT LTE GTE EQ NEQ NEG POS NOT NOP LINE BRA BRZ BRNZ BRZK BRNZK CALL RET RETV '
       'FOREACH POP POP2 DUP DUP2 SWAP PUSHNULL PUSHINT PUSHINT0 PUSHINT1 PUSHFP PUSHSTR '
       'PUSHTBL PUSHFN PUSHTHIS GETLOCAL SETLOCAL GETGLOBAL SETGLOBAL GETTHIS SETTHIS FORK').split()
OP = {n: i for i, n in enumerate(OPS)}
BRANCH = {OP[n] for n in ('BRA', 'BRZ', 'BRNZ', 'BRZK', 'BRNZK', 'FORK')}
SYMBOL = {OP[n] for n in ('GETDOT', 'SETDOT', 'GETTHIS', 'SETTHIS', 'GETGLOBAL', 'SETGLOBAL', 'PUSHSTR')}
OTHER_OPERAND = {OP[n] for n in ('PUSHINT', 'PUSHFP', 'CALL', 'GETLOCAL', 'SETLOCAL', 'PUSHFN', 'FOREACH')}  # FOREACH: two packed u16 locals
HAS_OPERAND = BRANCH | SYMBOL | OTHER_OPERAND


def parse(data):
    gid, flags, st_off, sc_off, fn_off = struct.unpack_from('<4sIIII', data, 0)
    if gid != b'0lmg':
        raise ValueError('bad header %r' % gid)
    st_size, = struct.unpack_from('<I', data, st_off)
    strtab = data[st_off + 4:st_off + 4 + st_size]
    starts = {0} | {i + 1 for i, c in enumerate(strtab[:-1]) if c == 0}
    nfuncs, = struct.unpack_from('<I', data, fn_off)
    o = fn_off + 4
    funcs = []
    for _ in range(nfuncs):
        tag, fid, root, nparams, nlocals, maxstack, bclen = struct.unpack_from('<4s6I', data, o)
        if tag != b'cnuf':
            raise ValueError('bad func tag at %x' % o)
        o += 28
        funcs.append(dict(id=fid, root=root, params=nparams, locals=nlocals, code=data[o:o + bclen]))
        o += bclen
        if flags & 1:  # debug info: name, line table, symbols
            o += 4
            nlines, = struct.unpack_from('<I', data, o)
            o += 4 + nlines * 8 + (nparams + nlocals) * 4
    return flags, strtab, starts, funcs


def check(data, hist, verbose=False):
    flags, strtab, starts, funcs = parse(data)
    errors = []
    for f in funcs:
        code = f['code']
        pcs, ins = set(), []
        pc = 0
        while pc < len(code):
            op, = struct.unpack_from('<I', code, pc)
            if op >= len(OPS):
                errors.append('func %d @%d: unknown opcode %d' % (f['id'], pc, op))
                break
            arg = struct.unpack_from('<I', code, pc + 4)[0] if op in HAS_OPERAND else None
            pcs.add(pc)
            ins.append((pc, op, arg))
            hist[OPS[op]] += 1
            pc += 8 if arg is not None else 4
        for pc, op, arg in ins:
            if op in BRANCH and arg not in pcs and arg != len(code):
                errors.append('func %d @%d: %s target %d not an instruction' % (f['id'], pc, OPS[op], arg))
            if op in SYMBOL and arg not in starts:
                errors.append('func %d @%d: %s symbol %d not a string start' % (f['id'], pc, OPS[op], arg))
            if op == OP['PUSHFN'] and arg >= len(funcs):
                errors.append('func %d @%d: PUSHFN %d out of range' % (f['id'], pc, arg))
            if verbose:
                s = ''
                if op in SYMBOL:
                    s = '"%s"' % strtab[arg:strtab.index(b'\0', arg)].decode('latin-1')
                elif op == OP['PUSHFP']:
                    s = '%g' % struct.unpack('<f', struct.pack('<I', arg))[0]
                elif arg is not None:
                    s = str(arg)
                print('  f%d %04d %-9s %s' % (f['id'], pc, OPS[op], s))
    return len(funcs), flags, errors


def main(argv):
    if '-v' in argv:
        check(open(argv[argv.index('-v') + 1], 'rb').read(), Counter(), verbose=True)
        return
    hist, nfiles, nfuncs, bad, debug = Counter(), 0, 0, [], 0
    for root, _, names in os.walk(argv[1]):
        for n in names:
            if n.endswith('.gm'):
                p = os.path.join(root, n)
                try:
                    k, flags, errs = check(open(p, 'rb').read(), hist)
                except Exception as e:
                    k, flags, errs = 0, 0, ['parse error: %s' % e]
                nfiles += 1
                nfuncs += k
                debug += flags & 1
                if errs:
                    bad.append((p, errs))
    print('%d files, %d functions, %d with debug info, %d files with errors' % (nfiles, nfuncs, debug, len(bad)))
    for p, errs in bad[:10]:
        print(' ', p, errs[:3])
    unused = [o for o in OPS if not hist[o]]
    print('instructions: %d; opcodes never used: %s' % (sum(hist.values()), ' '.join(unused) or 'none'))


if __name__ == '__main__':
    main(sys.argv)
