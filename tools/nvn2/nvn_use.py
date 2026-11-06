#!/usr/bin/env python3
"""NVN2 function use map of a game: which NVN2 functions the game's code calls (not just looks up), and which of them our runtime
implements.

Usage: python -I tools/nvn2/nvn_use.py --game <name> --module main=<path> [--module subsdk0=<path> ...] [--out-dir <dir>] [--runtime <dir>]

<path> is one of
  * an IDA database (.i64/.idb): a COPY is analysed (the original is never opened, so a database another session has open is fine)
  * an ELF: analysed by idat into <db-dir>/<module>.elf.i64 (kept, so the next run reuses it as a database)
  * an NSO/exefs module file: converted with toolchain/nx2elf into <db-dir>/<module>.elf first, then as an ELF
IDA Pro 9.4 runs headless (idat -A -S, IDA_DIR overrides the install path). Output (default <repo root>/work/audit):
  nvn_use_<game>.tsv         function, called (yes/no), call_sites, implemented (yes/stub), group, looked_up, read_sites, copies, modules
  nvn_use_<game>_summary.md  counts, stubbed functions the game calls by feature group, notes on what the scan could not follow

Method (ida_nvn_scan.py): the loader (the function that references the "nvn..." name strings) is read for name -> slot, the slot being
the global the proc pointer is stored in; every read of a slot is followed by a small dataflow (moves, selects, stack spills, address
arithmetic) to the instruction that uses the pointer: a BLR/BR is a call site; a store of the pointer into other memory is a copy of the
table, and when the destination is a resolved global its readers are scanned as well.
"""
import argparse, collections, json, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORKTREE = os.path.dirname(os.path.dirname(HERE))

GROUPS = [
    ('ray tracing', re.compile(r'AccelerationStructure|DispatchRays|RayTracing|TraceRecursionDepth|CallableRecursionDepth|ShaderGroupHandle')),
    ('mesh', re.compile(r'MeshTasks')),
    ('DLSS', re.compile(r'Dlss')),
    ('device-generated commands', re.compile(r'DeviceCommand|DeviceCommands|GetCommandHandleMemorySize')),
    ('VRS', re.compile(r'ShadingRate')),
    ('push constants', re.compile(r'Push(Graphics|Compute)Constants')),
    ('ProgramBuilder', re.compile(r'ProgramBuilder')),
    ('dirty tile map', re.compile(r'DirtyTileMap')),
    ('message buffer', re.compile(r'MessageBuffer')),
]


def group_of(name):
    for g, rx in GROUPS:
        if rx.search(name):
            return g
    return 'other'


def repo_root():
    d = HERE
    while True:
        if os.path.isdir(os.path.join(d, 'work')) and os.path.isdir(os.path.join(d, '.weaver')):
            return d
        p = os.path.dirname(d)
        if p == d:
            return None
        d = p


def runtime_tables(runtime):
    """(all NVN2 proc names in table order, set of implemented names) from the runtime source."""
    tsv = os.path.join(runtime, 'src', 'gfx', 'nvn', 'nvn2_procs.tsv')
    names = []
    with open(tsv, encoding='utf-8') as f:
        for ln in f:
            if ln.startswith('#') or ln.startswith('idx\t'):
                continue
            c = ln.rstrip('\n').split('\t')
            if len(c) > 1 and c[1].startswith('nvn'):
                names.append(c[1])
    impl = {'nvnBootstrapLoader', 'nvnDeviceGetProcAddress'}      # real implementations from the start (nvn_loader.cpp install())
    rx = re.compile(r'\bNVN2?_REG\((nvn[A-Za-z0-9_]+)\)')
    for dp, _, fs in os.walk(os.path.join(runtime, 'src')):
        for fn in fs:
            if fn.endswith(('.cpp', '.h', '.inc')):
                with open(os.path.join(dp, fn), encoding='utf-8', errors='replace') as f:
                    for ln in f:
                        if ln.lstrip().startswith('//'):
                            continue
                        impl.update(rx.findall(ln))
    return names, impl


def find_idat():
    d = os.environ.get('IDA_DIR') or r'C:\Program Files\IDA Professional 9.4'
    p = os.path.join(d, 'idat.exe')
    if not os.path.exists(p):
        sys.exit('idat.exe not found in ' + d + ' (set IDA_DIR)')
    return p


def run_idat(idat, db_or_elf, out_json, log, create):
    env = dict(os.environ)
    env['NVN2_SCAN_OUT'] = out_json
    env['NVN2_SCAN_DIR'] = HERE
    if create:
        env['NVN2_SCAN_KEEP'] = '1'      # keep the database made from an ELF: analysis is the slow part
    cmd = [idat, '-A', '-L' + log, '-S' + os.path.join(HERE, 'ida_driver.py')]
    if create:
        cmd.insert(1, '-c')
    cmd.append(db_or_elf)
    # below-normal priority: the PC is shared
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=0x4000 if os.name == 'nt' else 0)
    if os.path.exists(out_json + '.err'):
        sys.exit('IDA script failed:\n' + open(out_json + '.err', encoding='utf-8').read())
    if not os.path.exists(out_json):
        sys.exit('no output from idat; see ' + log)
    with open(out_json, encoding='utf-8') as f:
        return json.load(f)


def scan_module(modname, path, db_dir, tmp, idat):
    path = os.path.abspath(path)
    out_json = os.path.join(tmp, modname + '.json')
    log = os.path.join(tmp, modname + '.log')
    low = path.lower()
    if low.endswith(('.i64', '.idb')):
        cp = os.path.join(tmp, modname + os.path.splitext(path)[1])
        print(f'[{modname}] copying database {path}')
        shutil.copyfile(path, cp)
        return run_idat(idat, cp, out_json, log, False)
    elf = path
    if not low.endswith('.elf'):
        elf = os.path.join(db_dir, modname + '.elf')
        os.makedirs(db_dir, exist_ok=True)
        if not os.path.exists(elf):
            nx2elf = os.path.join(os.path.dirname(WORKTREE), '..', 'toolchain', 'nx2elf', 'nx2elf.exe')
            root = repo_root()
            if root:
                nx2elf = os.path.join(root, 'toolchain', 'nx2elf', 'nx2elf.exe')
            print(f'[{modname}] nx2elf {path}')
            r = subprocess.run([nx2elf, path, '--export-elf', elf], capture_output=True, text=True)
            if not os.path.exists(elf):
                sys.exit('nx2elf failed:\n' + r.stdout + r.stderr)
    db = elf + '.i64'
    if os.path.exists(db):
        print(f'[{modname}] reusing database {db}')
        return run_idat(idat, db, out_json, log, False)
    print(f'[{modname}] analysing {elf} (can take a long time)')
    if os.path.abspath(os.path.dirname(elf)) != os.path.abspath(db_dir):
        os.makedirs(db_dir, exist_ok=True)
        cp = os.path.join(db_dir, modname + '.elf')
        shutil.copyfile(elf, cp)
        elf = cp
    return run_idat(idat, elf, out_json, log, True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', required=True)
    ap.add_argument('--module', action='append', required=True, metavar='NAME=PATH')
    ap.add_argument('--out-dir', default=None)
    ap.add_argument('--db-dir', default=None, help='where new databases/ELFs go (default <repo root>/work/ida/<game>)')
    ap.add_argument('--runtime', default=os.path.join(WORKTREE, 'runtime'), help='runtime source tree that says what is implemented')
    ap.add_argument('--keep-json', action='store_true', help='keep the raw per-module scan results as <out-dir>/nvn_use_<game>_<module>.json')
    a = ap.parse_args()

    root = repo_root()
    out_dir = os.path.abspath(a.out_dir or os.path.join(root or '.', 'work', 'audit'))
    db_dir = os.path.abspath(a.db_dir or os.path.join(root or '.', 'work', 'ida', a.game))
    os.makedirs(out_dir, exist_ok=True)
    names, impl = runtime_tables(a.runtime)
    idat = find_idat()

    tmp_parent = os.path.join(root, 'work', 'ida') if root else None
    os.makedirs(tmp_parent, exist_ok=True) if tmp_parent else None
    tmp = tempfile.mkdtemp(prefix='_nvn_use_', dir=tmp_parent)
    results = {}
    try:
        for m in a.module:
            nm, _, p = m.partition('=')
            if not p:
                sys.exit('--module needs NAME=PATH')
            results[nm] = scan_module(nm, p, db_dir, tmp, idat)
            if a.keep_json:
                shutil.copyfile(os.path.join(tmp, nm + '.json'), os.path.join(out_dir, f'nvn_use_{a.game}_{nm}.json'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ---- merge ----
    allnames = list(names) + sorted({n for r in results.values() for n in r['procs']} - set(names))
    rows = []
    for n in allnames:
        sites, reads, copies, unres, passed, mods, looked = 0, 0, 0, 0, 0, [], False
        for mn, r in results.items():
            p = r['procs'].get(n)
            if not p:
                continue
            looked = True
            sites += len(p['calls']); reads += len(p['loads']); copies += len(p['copies']); unres += len(p['unresolved_copies']); passed += len(p['passed'])
            if p['calls']:
                mods.append(f"{mn}:{len(p['calls'])}")
        rows.append(dict(function=n, called='yes' if sites else 'no', call_sites=sites, implemented='yes' if n in impl else 'stub',
                         group=group_of(n), looked_up='yes' if looked else 'no', read_sites=reads, copies=copies, unresolved=unres,
                         passed=passed, modules=','.join(mods), in_table='yes' if n in names else 'no'))
    cols = ['function', 'called', 'call_sites', 'implemented', 'group', 'looked_up', 'read_sites', 'copies', 'modules']
    tsv = os.path.join(out_dir, f'nvn_use_{a.game}.tsv')
    with open(tsv, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\t'.join(cols) + '\n')
        for r in rows:
            f.write('\t'.join(str(r[c]) for c in cols) + '\n')

    called = [r for r in rows if r['called'] == 'yes']
    looked = [r for r in rows if r['looked_up'] == 'yes']
    stub_called = [r for r in called if r['implemented'] == 'stub']
    stub_looked_only = [r for r in looked if r['called'] == 'no' and r['implemented'] == 'stub']
    md = [f'# NVN2 use map: {a.game}', '',
          'Modules scanned: ' + ', '.join(f"{m} ({r['slots']} proc lookups, loader {', '.join(f'{x:#x}-{y:#x}' for x, y in r['loader_ranges']) or 'none'})" for m, r in results.items()), '',
          f'- Functions looked up by name: {len(looked)} (of {len(names)} in the NVN2 table)',
          f'- Called (a BLR/BR on the looked-up pointer, followed by dataflow): {len(called)}, {sum(r["call_sites"] for r in called)} call sites',
          f'- Called and implemented in our runtime: {len(called) - len(stub_called)}; called and still a logging stub: {len(stub_called)}',
          f'- Looked up, never called, stub: {len(stub_looked_only)}',
          f'- Copies of the pointer stored into memory: {sum(1 for r in rows if r["copies"])} functions ({sum(r["copies"] for r in rows)} stores; '
          f'{sum(1 for r in rows if r["unresolved"])} with a destination the scan could not resolve); passed as a call argument: {sum(1 for r in rows if r["passed"])}',
          '- "Implemented" = registered with NVN_REG / NVN2_REG in the runtime source (or the two loader functions); partial or ignoring implementations count as implemented.', '']
    md += ['## Stubbed functions the game calls, by feature group', '']
    if stub_called:
        byg = collections.defaultdict(list)
        for r in stub_called:
            byg[r['group']].append(r)
        order = [g for g, _ in GROUPS] + ['other']
        for g in order:
            if g in byg:
                md.append(f'### {g} ({len(byg[g])})')
                md.append('')
                md.append(', '.join(f"{r['function']} ({r['call_sites']})" for r in sorted(byg[g], key=lambda r: r['function'])))
                md.append('')
    else:
        md += ['None: every function the game calls is implemented.', '']
    md += ['## Stubbed, looked up but never called (by group)', '']
    byg = collections.Counter(r['group'] for r in stub_looked_only)
    md.append(', '.join(f'{g} {byg[g]}' for g in [g for g, _ in GROUPS] + ['other'] if byg[g]) or 'none')
    md.append('')
    notgot = [r for r in rows if r['looked_up'] == 'no' and r['in_table'] == 'yes']
    md += ['## Not looked up', '', f'{len(notgot)} table functions are never requested by name in the scanned modules.', '']
    unres = [r for r in rows if r['unresolved']]
    if unres:
        md += ['## Could not follow', '', 'Pointer stored to memory whose address the scan could not resolve (calls through that copy are not counted): ' +
               ', '.join(r['function'] for r in unres[:30]) + ('...' if len(unres) > 30 else ''), '']
    with open(os.path.join(out_dir, f'nvn_use_{a.game}_summary.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(md))
    print(f'wrote {tsv}: {len(looked)} looked up, {len(called)} called ({len(stub_called)} stub)')


if __name__ == '__main__':
    main()
