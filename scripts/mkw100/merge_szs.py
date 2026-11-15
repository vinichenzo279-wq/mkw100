#!/usr/bin/env python3
"""Merge U8 archives (.szs, Yaz0 or plain) into one; later archives win on equal paths.

    python -I scripts/mkw100/merge_szs.py --out merged.szs base.szs overlay1.szs [overlay2.szs ...]

An archive given as PATH::GLOB contributes only the files whose path matches GLOB.

Used for Retro Rewind + Item Fever items (docs/mkw100/rr-item-fever.md): Item Fever loads its
CommonAssets2.szs on top of Race/Common.szs through an archive hook that the combined build leaves
to Retro Rewind, so the stage gets one Race/Common.szs holding both. The output is written with
Yaz0 compression. Paths are compared without a leading "./".
"""
import argparse
import struct


def yaz0_decode(d):
    if d[:4] != b'Yaz0':
        return d
    n = struct.unpack('>I', d[4:8])[0]
    out = bytearray()
    p = 16
    while len(out) < n:
        h = d[p]
        p += 1
        for _ in range(8):
            if len(out) >= n:
                break
            if h & 0x80:
                out.append(d[p])
                p += 1
            else:
                b1, b2 = d[p], d[p + 1]
                p += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                count = b1 >> 4
                if count == 0:
                    count = d[p] + 0x12
                    p += 1
                else:
                    count += 2
                for _ in range(count):
                    out.append(out[-dist])
            h = (h << 1) & 0xFF
    return bytes(out)


def yaz0_encode(data):
    """Yaz0 with a hash-chain match finder (window 4 KiB, matches 3..273 bytes)."""
    n = len(data)
    out = bytearray(b'Yaz0' + struct.pack('>I', n) + bytes(8))
    heads = {}
    prev = [0] * n
    pos = 0

    def insert(i):
        if i + 3 <= n:
            key = data[i:i + 3]
            prev[i] = heads.get(key, -1)
            heads[key] = i

    while pos < n:
        flag_at = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if pos >= n:
                break
            best_len, best_dist = 0, 0
            if pos + 3 <= n:
                cand = heads.get(data[pos:pos + 3], -1)
                tries = 32
                limit = min(273, n - pos)
                while cand >= 0 and pos - cand <= 0x1000 and tries:
                    length = 3
                    while length < limit and data[cand + length] == data[pos + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_dist = length, pos - cand
                        if length == limit:
                            break
                    cand = prev[cand]
                    tries -= 1
            if best_len >= 3:
                d = best_dist - 1
                if best_len >= 0x12:
                    out += bytes(((d >> 8) & 0xF, d & 0xFF, best_len - 0x12))
                else:
                    out += bytes((((best_len - 2) << 4) | ((d >> 8) & 0xF), d & 0xFF))
                for i in range(pos, pos + best_len):
                    insert(i)
                pos += best_len
            else:
                flags |= 0x80 >> bit
                out.append(data[pos])
                insert(pos)
                pos += 1
        out[flag_at] = flags
    return bytes(out)


def u8_files(d):
    """{path: bytes} of a U8 archive, paths without the root '.' component."""
    if d[:4] != b'\x55\xaa\x38\x2d':
        raise SystemExit('not a U8 archive')
    root = struct.unpack('>I', d[4:8])[0]

    def node(i):
        return struct.unpack('>III', d[root + i * 12:root + i * 12 + 12])
    _, _, count = node(0)
    strings = root + count * 12
    files = {}
    path, ends = [], [count]
    for i in range(1, count):
        w0, off, size = node(i)
        while i >= ends[-1]:
            ends.pop()
            path.pop()
        e = d.find(b'\0', strings + (w0 & 0xFFFFFF))
        name = d[strings + (w0 & 0xFFFFFF):e].decode('latin1')
        if w0 >> 24:
            ends.append(size)
            path.append(name)
        else:
            parts = [p for p in path + [name] if p and p != '.']
            files['/'.join(parts)] = d[off:off + size]
    return files


def u8_build(files):
    """A U8 archive with every file under a root '.' directory (the layout of the game's own)."""
    tree = {}
    for path, data in files.items():
        node = tree
        parts = path.split('/')
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = data
    nodes, names = [], bytearray(b'\0')

    def add(name, node, parent):
        index = len(nodes)
        name_off = len(names)
        names.extend(name.encode('latin1') + b'\0')
        if isinstance(node, dict):
            nodes.append(['dir', name_off, parent, 0])
            for child in sorted(node, key=lambda s: s.lower()):
                add(child, node[child], index)
            nodes[index][3] = len(nodes)
        else:
            nodes.append(['file', name_off, node, None])
    nodes.append(['dir', 0, 0, 0])  # root
    add('.', tree, 0)
    nodes[0][3] = len(nodes)
    header_size = 0x20
    table_size = len(nodes) * 12 + len(names)
    data_start = (header_size + table_size + 0x1F) & ~0x1F
    blob = bytearray()
    entries = bytearray()
    for kind, name_off, a, b in nodes:
        if kind == 'dir':
            entries += struct.pack('>III', (1 << 24) | name_off, a, b)
        else:
            off = data_start + len(blob)
            entries += struct.pack('>III', name_off, off, len(a))
            blob += a
            blob += bytes((-len(blob)) % 0x20)
    header = struct.pack('>IIII', 0x55AA382D, header_size, table_size, data_start) + bytes(16)
    out = bytearray(header + entries + names)
    out += bytes(data_start - len(out))
    return bytes(out + blob)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('archives', nargs='+')
    a = ap.parse_args()
    merged = {}
    import fnmatch
    for spec in a.archives:
        path, _, pattern = spec.partition('::')
        files = u8_files(yaz0_decode(open(path, 'rb').read()))
        if pattern:
            files = {k: v for k, v in files.items() if fnmatch.fnmatch(k, pattern)}
        replaced = sum(1 for k in files if k in merged)
        merged.update(files)
        print(f'{path}: {len(files)} files, {replaced} replace earlier ones')
    with open(a.out, 'wb') as f:
        f.write(yaz0_encode(u8_build(merged)))
    print(f'{a.out}: {len(merged)} files')


if __name__ == '__main__':
    main()
