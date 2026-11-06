#!/usr/bin/env python3
"""Translation regression gate: which SPIR-V blocks changed between two shader caches.

  cache_diff.py manifest <cache_dir>            write <cache_dir>/manifest.tsv (name, size, blake2b) - reuses rows whose size and mtime match
  cache_diff.py expect <blocks.tsv> <regex> --out FILE   blocks whose op list (census.py --keep: <game>_blocks.tsv, ops column) matches regex
  cache_diff.py diff <old_dir|old.tsv> <new_dir|new.tsv> [--expect FILE] [--out FILE]

diff lists changed, added and removed blocks. With --expect (one block name per line, the blocks a translator fix is meant to change),
any changed block not in the list fails the gate (exit 1). Without it the diff is only reported.
"""
import argparse
import hashlib
import os
import sys
from concurrent.futures import ThreadPoolExecutor


def _hash(path):
    h = hashlib.blake2b(digest_size=16)
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def manifest(cache_dir):
    spv_dir = os.path.join(cache_dir, 'spirv') if os.path.isdir(os.path.join(cache_dir, 'spirv')) else cache_dir
    out = os.path.join(cache_dir, 'manifest.tsv')
    old = {}
    if os.path.exists(out):
        for line in open(out, encoding='utf-8'):
            n, size, mtime, digest = line.rstrip('\n').split('\t')
            old[n] = (int(size), int(mtime), digest)
    entries = [e for e in os.scandir(spv_dir) if e.is_file() and e.name.endswith('.spv')]
    rows, todo = {}, []
    for e in entries:
        st = e.stat()
        prev = old.get(e.name)
        if prev and prev[0] == st.st_size and prev[1] == st.st_mtime_ns:
            rows[e.name] = prev
        else:
            todo.append((e.name, e.path, st.st_size, st.st_mtime_ns))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as ex:
        for (n, _, size, mtime), digest in zip(todo, ex.map(lambda t: _hash(t[1]), todo)):
            rows[n] = (size, mtime, digest)
    with open(out, 'w', encoding='utf-8') as f:
        for n in sorted(rows):
            size, mtime, digest = rows[n]
            f.write(f'{n}\t{size}\t{mtime}\t{digest}\n')
    print(f'{out}: {len(rows)} blocks ({len(todo)} hashed now)')
    return out


def load(p):
    if os.path.isdir(p):
        p = manifest(p)
    return {n: d for n, _, _, d in (line.rstrip('\n').split('\t') for line in open(p, encoding='utf-8'))}


def diff(a, b, expect, out):
    old, new = load(a), load(b)
    changed = sorted(n for n in old.keys() & new.keys() if old[n] != new[n])
    added = sorted(new.keys() - old.keys())
    removed = sorted(old.keys() - new.keys())
    lines = [f'C\t{n}' for n in changed] + [f'A\t{n}' for n in added] + [f'R\t{n}' for n in removed]
    if out:
        open(out, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print(f'changed {len(changed)}, added {len(added)}, removed {len(removed)} (of {len(old)} -> {len(new)})')
    if expect is None:
        return 0
    allowed = {l.strip() for l in open(expect, encoding='utf-8') if l.strip()}
    unexpected = [n for n in changed if n not in allowed and n.removesuffix('.spv') not in allowed]
    print(f'unexpected changes: {len(unexpected)}')
    for n in unexpected[:20]:
        print('  ' + n)
    return 1 if unexpected else 0


def expect(blocks, regex, out):
    import re
    rx = re.compile(regex)
    hits = []
    with open(blocks, encoding='utf-8') as f:
        head = f.readline().rstrip('\n').split('\t')
        oi = head.index('ops')
        for line in f:
            c = line.rstrip('\n').split('\t')
            if len(c) > oi and any(rx.search(op) for op in c[oi].split(' ')):
                hits.append(c[0])
    open(out, 'w', encoding='utf-8').write('\n'.join(hits) + '\n')
    print(f'{len(hits)} blocks match {regex}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('manifest')
    m.add_argument('cache_dir')
    d = sub.add_parser('diff')
    d.add_argument('old')
    d.add_argument('new')
    d.add_argument('--expect')
    d.add_argument('--out')
    e = sub.add_parser('expect')
    e.add_argument('blocks')
    e.add_argument('regex')
    e.add_argument('--out', required=True)
    a = ap.parse_args()
    if a.cmd == 'expect':
        return expect(a.blocks, a.regex, a.out)
    if a.cmd == 'manifest':
        manifest(a.cache_dir)
        return 0
    return diff(a.old, a.new, a.expect, a.out)


if __name__ == '__main__':
    sys.exit(main())
