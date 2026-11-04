#!/usr/bin/env python3
"""Extract every file from the PS2 ISO (ISO9660) into a folder.
Usage: python tools/extract_iso.py "<game.iso>" extracted/disc
"""
import os, struct, sys

SECTOR = 2048

def main(iso_path, out_dir):
    f = open(iso_path, 'rb')
    def read(lba, n):
        f.seek(lba * SECTOR)
        return f.read(n)

    def walk(lba, size, rel):
        data = read(lba, size)
        i = 0
        while i < len(data):
            length = data[i]
            if length == 0:  # records never cross sector boundaries
                i = (i // SECTOR + 1) * SECTOR
                continue
            rec = data[i:i + length]
            i += length
            name = rec[33:33 + rec[32]]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('ascii').split(';')[0]
            e_lba, = struct.unpack_from('<I', rec, 2)
            e_size, = struct.unpack_from('<I', rec, 10)
            path = os.path.join(rel, name)
            if rec[25] & 2:
                os.makedirs(os.path.join(out_dir, path), exist_ok=True)
                walk(e_lba, e_size, path)
            else:
                print('%11d  %s' % (e_size, path))
                f.seek(e_lba * SECTOR)
                with open(os.path.join(out_dir, path), 'wb') as o:
                    left = e_size
                    while left:
                        chunk = f.read(min(left, 1 << 20))
                        o.write(chunk)
                        left -= len(chunk)

    pvd = read(16, SECTOR)
    assert pvd[1:6] == b'CD001', 'not an ISO9660 image'
    root = pvd[156:190]
    os.makedirs(out_dir, exist_ok=True)
    walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
