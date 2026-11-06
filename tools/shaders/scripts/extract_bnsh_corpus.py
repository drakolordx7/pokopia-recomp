#!/usr/bin/env python3
"""Pull the NVN2 shader blocks (control section + code area) out of Nintendo shader archives (BFSHA/BNSH, Nintendo EPD engine:
Donkey Kong Bananza and others) into the census corpus layout (manifest.tsv, code_<h>.bin, ctrl_<h>.bin).

Usage: python -I extract_bnsh_corpus.py <file or dir> [<file or dir> ...] --out <dir>
  inputs: .bfsha / .sarc / .bnsh files, or their .zs (zstd) form; a directory is searched recursively for *Nin_Ounce_NVN2* files
  (Shader/*.bfsha.zs, Shader/Project.*.sarc.zs). Zstd needs Python 3.14 (compression.zstd) or the `zstandard` package:
  `py -V:Astral/CPython3.14.7 -I tools/shaders/scripts/extract_bnsh_corpus.py ...`.

Method (found by reading DKB's files, no format spec used): inside every 'BNSH' region an NVN2 control section starts with
u32 0x98761234 (+0x04 = 0xc9, length u32 at +0x30, code area length u32 at +0x44). Each section is referenced by a 24-byte program
record in the same region: {u64 control offset, u64 code offset, u32 code size, u32 control size}, offsets relative to the 'BNSH'
magic. A record is accepted only when code size equals the control's +0x44 and control size equals its +0x30, and the code lies inside
the region. Code blocks are content-addressed by the first 16 hex digits of sha1(code). Nothing is written next to the inputs.
"""
import argparse, array, hashlib, os, struct, sys

CTRL_MAGIC = b'\x34\x12\x76\x98'
STAGES = {0: 'vertex', 1: 'fragment', 2: 'geometry', 3: 'tess_control', 4: 'tess_eval', 5: 'compute'}


def zstd_decompress(b):
    try:
        from compression import zstd            # Python 3.14
        return zstd.decompress(b)
    except ImportError:
        try:
            import zstandard
            return zstandard.ZstdDecompressor().decompressobj().decompress(b)
        except ImportError:
            sys.exit('zstd needed: run with Python 3.14 (compression.zstd) or install zstandard')


def load(path):
    d = open(path, 'rb').read()
    if d[:4] == b'\x28\xb5\x2f\xfd':
        d = zstd_decompress(d)
    return d


def find_all(d, pat):
    out, i = [], d.find(pat)
    while i >= 0:
        out.append(i)
        i = d.find(pat, i + 1)
    return out


def process(d, blocks, stats):
    bnsh = find_all(d, b'BNSH')
    bnsh = [b for b in bnsh if b % 0x10 == 0]
    if not bnsh:
        return
    ctrls = []
    for h in find_all(d, CTRL_MAGIC):
        if h + 0x58 > len(d) or struct.unpack_from('<I', d, h + 4)[0] != 0xc9:
            continue
        cl = struct.unpack_from('<I', d, h + 0x30)[0]
        if not 0xd8 <= cl <= 0x1000 or h + cl > len(d):
            continue
        ctrls.append(h)
    stats['ctrl'] += len(ctrls)
    for k, bs in enumerate(bnsh):
        be = bnsh[k + 1] if k + 1 < len(bnsh) else len(d)
        mine = [h for h in ctrls if bs <= h < be]
        if not mine:
            continue
        want = {h - bs: h for h in mine}
        region = memoryview(d)[bs:be]
        n8 = len(region) // 8
        arr = array.array('Q')
        arr.frombytes(region[:n8 * 8].tobytes())
        found = {}
        for i, v in enumerate(arr):
            if v in want and v not in found and i + 2 < n8:
                code_off = arr[i + 1]
                code_size, ctrl_size = struct.unpack_from('<II', region, (i + 2) * 8)
                h = want[v]
                if code_size == struct.unpack_from('<I', d, h + 0x44)[0] and ctrl_size == struct.unpack_from('<I', d, h + 0x30)[0] \
                        and code_off + code_size <= len(region) and code_size > 0:
                    found[v] = (code_off, code_size, ctrl_size)
        for rel, (code_off, code_size, ctrl_size) in found.items():
            h = want[rel]
            code = bytes(region[code_off:code_off + code_size])
            ctrl = bytes(d[h:h + ctrl_size])
            blocks.append((hashlib.sha1(code).hexdigest()[:16], code, ctrl, struct.unpack_from('<H', ctrl, 0x56)[0]))
        stats['matched'] += len(found)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    files = []
    for p in a.inputs:
        if os.path.isdir(p):
            for dp, _, fs in os.walk(p):
                files += [os.path.join(dp, f) for f in sorted(fs) if 'Nin_Ounce_NVN2' in f]
        else:
            files.append(p)
    os.makedirs(a.out, exist_ok=True)
    blocks, stats = [], {'ctrl': 0, 'matched': 0}
    for f in sorted(files):
        before = stats['matched']
        process(load(f), blocks, stats)
        print(f'{f}: {stats["matched"] - before} blocks')
    seen, rows = set(), []
    for h, code, ctrl, st in sorted(blocks, key=lambda b: b[0]):
        if h in seen:
            continue
        seen.add(h)
        open(os.path.join(a.out, f'code_{h}.bin'), 'wb').write(code)
        open(os.path.join(a.out, f'ctrl_{h}.bin'), 'wb').write(ctrl)
        ps, pb = struct.unpack_from('<II', ctrl, 0x30)[0], struct.unpack_from('<I', ctrl, 0x38)[0]
        rows.append(f'bnsh_{h}\t{STAGES.get(st, "unknown")}\t0x0\t0x{len(code):x}\t0x0\t0x{len(ctrl):x}\t0x0\t0x{pb:x}\t0x0\t0x0\t{h}\tcode_{h}.bin\tctrl_{h}.bin')
    with open(os.path.join(a.out, 'manifest.tsv'), 'w', newline='\n') as f:
        f.write('id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\tconst_size\tcode_sha1_16\tcode_file\tctrl_file\n')
        f.write('\n'.join(rows) + '\n')
    print(f'control sections found {stats["ctrl"]}, with a program record {stats["matched"]}, distinct code blocks {len(seen)} -> {a.out}')


if __name__ == '__main__':
    main()
