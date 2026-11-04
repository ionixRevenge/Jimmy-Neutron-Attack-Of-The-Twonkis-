#!/usr/bin/env python3
"""Extract a Twonkies .PAK archive.

Format (little-endian), reverse-engineered from the disc:
  0x00 char[4] magic  "kcap"
  0x04 u32     version (1)
  0x08 u32     name table size
  0x0C u32     total archive size
  0x10 u32     name table offset
  0x14 u32     file count
  0x18 entry[count] { u32 name_offset; u32 data_offset; u32 size; }
  name table: NUL-terminated strings; name_offset is relative to its start.
  File data is 2048-byte (sector) aligned.
Usage: python tools/extract_pak.py FILE.PAK out_dir   (or a folder of PAKs)
"""
import os, struct, sys

def extract(pak_path, out_dir):
    with open(pak_path, 'rb') as f:
        magic, ver, names_size, total, names_off, count = struct.unpack('<4sIIIII', f.read(0x18))
        assert magic == b'kcap', '%s: bad magic %r' % (pak_path, magic)
        entries = [struct.unpack('<III', f.read(12)) for _ in range(count)]
        f.seek(names_off)
        names = f.read(names_size)
        for name_off, off, size in entries:
            name = names[name_off:names.index(b'\0', name_off)].decode('latin-1')
            name = name.replace('\\', '/').lstrip('/')
            dst = os.path.join(out_dir, name)
            os.makedirs(os.path.dirname(dst) or out_dir, exist_ok=True)
            f.seek(off)
            with open(dst, 'wb') as o:
                o.write(f.read(size))
        print('%-14s v%d  %5d files' % (os.path.basename(pak_path), ver, count))

if __name__ == '__main__':
    src, out = sys.argv[1], sys.argv[2]
    if os.path.isdir(src):
        for n in sorted(os.listdir(src)):
            if n.upper().endswith('.PAK'):
                extract(os.path.join(src, n), os.path.join(out, os.path.splitext(n)[0].lower()))
    else:
        extract(src, out)
