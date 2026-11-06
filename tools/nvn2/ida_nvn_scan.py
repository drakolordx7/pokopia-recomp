"""IDAPython part of the NVN2 function use map (run through tools/nvn2/nvn_use.py, or exec'd in an open database).

For the module open in IDA (AArch64 ELF made from an NSO):
  1. Loader: every instruction that references a string "nvn<Name>" is followed (linear scan) to the next BLR (the
     nvnDeviceGetProcAddress call) and then to the first `STR X0,[Xn]` before the next BLR: Xn is the slot the proc pointer is
     stored in (a GOT entry holding the address of the global, or the global itself). Result: name -> slot.
  2. Uses: every instruction that reads a slot (xrefs to the GOT entry or to the global) starts a small forward dataflow over the
     control flow graph (budget per site): registers hold either "address of slot N" or "proc pointer N"; moves, loads through the
     address, spills to and reloads from the stack frame are followed; callee-saved registers survive BL. The proc pointer ends in
       call   BLR/BR on it                       (call-site address recorded)
       copy   stored somewhere that is not the stack frame (a table the game fills; the store address is recorded, so the table's
              own slot can be followed: see follow_copies)
       passed in x0..x7 at a BL                  (handed to another function)
  3. Copies into global memory (store address resolved from ADRP/ADD or a GOT load) become new slots of the same name and are
     scanned the same way (copies of the table), a few rounds.
Everything is plain facts about code addresses; the module name and the output path come from the caller.
"""
import re, json, collections
import idc, idautils, ida_bytes, ida_funcs, ida_ua, ida_segment, ida_ida, idaapi

NAME_RE = re.compile(r'^nvn[A-Z][A-Za-z0-9_]*$')
COND_BR = ('B.', 'CBZ', 'CBNZ', 'TBZ', 'TBNZ')
STORE_NO_DEST = ('STR', 'STP', 'STUR', 'STRB', 'STRH', 'STLR', 'STNP', 'CMP', 'CMN', 'TST', 'CCMP', 'CCMN', 'PRFM', 'NOP')
BUDGET = 90
ROUNDS = 3


def line(ea):
    s = idc.generate_disasm_line(ea, 0) or ''
    return s.split(';')[0].strip()


def split_ops(s):
    """'LDR X8, [X8,#off]' -> ('LDR', ['X8', '[X8,#off]'])"""
    m = re.match(r'(\S+)\s*(.*)', s)
    if not m:
        return '', []
    mn, rest = m.group(1).upper(), m.group(2)
    ops, depth, cur = [], 0, ''
    for ch in rest:
        if ch in '[{(':
            depth += 1
        elif ch in ']})':
            depth -= 1
        if ch == ',' and depth == 0:
            ops.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return mn, ops


def reg(o):
    """Canonical register id of an operand text ('X8', 'W8' -> 8; 'SP' -> 'sp'; 'XZR' -> None), else None."""
    o = o.strip().upper()
    if o in ('SP', 'WSP'):
        return 'sp'
    m = re.match(r'^[XW](\d+)$', o)
    if m:
        return int(m.group(1))
    return None


def mem(o):
    """'[X8,#0x10]' -> (base, offset) ; '[X8]' -> (8, 0); other forms -> (None, None)"""
    m = re.match(r'^\[\s*([A-Za-z0-9]+)\s*(?:,\s*#?(-?(?:0x)?[0-9A-Fa-f]+))?\s*\](!?)$', o.strip())
    if not m:
        m2 = re.match(r'^\[\s*([A-Za-z0-9]+)\s*,\s*#[A-Za-z_0-9.$]+@PAGEOFF\s*\]$', o.strip())   # symbolic page offset
        if m2:
            return reg(m2.group(1)), 'sym'
        return None, None
    b = reg(m.group(1))
    off = 0
    if m.group(2):
        try:
            off = int(m.group(2), 0) if m.group(2).lower().startswith(('0x', '-0x')) else int(m.group(2), 16 if re.search('[a-fA-F]', m.group(2)) else 10)
        except ValueError:
            off = None
    return b, off


def get_image_base():
    return ida_ida.inf_get_min_ea() & ~0xfff


def targets(ea):
    """Branch targets of the instruction at ea (o_near operands)."""
    insn = ida_ua.insn_t()
    if not ida_ua.decode_insn(insn, ea):
        return []
    return [op.addr for op in insn.ops if op.type == ida_ua.o_near]


# ---------------------------------------------------------------------------------------------------------------- loader
def find_slots():
    """name -> {'got': ea of the GOT entry or None, 'addr': address of the global holding the proc pointer, 'store': ea of the STR}."""
    base = get_image_base()
    strs = {}
    for st in idautils.Strings():
        s = str(st)
        if NAME_RE.match(s):
            strs[st.ea] = s
    refs = []   # (ea of referencing instruction, name)
    for sea, name in strs.items():
        for x in idautils.XrefsTo(sea):
            refs.append((x.frm, name))
    refs.sort()
    slots = {}
    loader_ranges = []
    # group by containing function; scan each function's instruction stream once
    byfunc = collections.defaultdict(list)
    for ea, name in refs:
        f = ida_funcs.get_func(ea)
        byfunc[f.start_ea if f else 0].append((ea, name))
    for fstart, lst in byfunc.items():
        if len(lst) < 8:        # the loader names hundreds of procs; skip incidental references
            continue
        f = ida_funcs.get_func(lst[0][0])
        refset = {ea: n for ea, n in lst}
        cur_name = None
        pending = None          # name whose BLR was seen, waiting for the STR x0
        regs = {}               # reg -> ('got', ea) | ('addr', ea)
        ea = f.start_ea
        first_ref = last_ref = None
        while ea < f.end_ea:
            s = line(ea)
            mn, ops = split_ops(s)
            # string reference: ADRL/ADR/ADD with the xref recorded at one of its instructions
            if ea in refset:
                cur_name = refset[ea]
                first_ref = first_ref or ea
                last_ref = ea
            elif mn == 'ADRL' or (mn in ('ADD', 'ADR') and len(ops) >= 2):
                for x in idautils.XrefsFrom(ea, 0):
                    if x.to in strs:
                        cur_name = strs[x.to]
            if mn == 'BLR':
                pending = cur_name
                cur_name = None
            elif mn in ('ADRP',) and len(ops) >= 2:
                regs[reg(ops[0])] = ('page', None)
            elif mn == 'LDR' and len(ops) == 2 and reg(ops[0]) is not None and '@PAGEOFF' in ops[1]:
                sym = re.search(r'#([A-Za-z_0-9.$]+)@PAGEOFF', ops[1]).group(1)
                a = idc.get_name_ea_simple(sym)
                regs[reg(ops[0])] = ('got', a) if a != idc.BADADDR else ('?', None)
            elif mn == 'ADD' and len(ops) == 3 and '@PAGEOFF' in ops[2]:
                sym = re.search(r'#([A-Za-z_0-9.$]+)@PAGEOFF', ops[2]).group(1)
                a = idc.get_name_ea_simple(sym)
                regs[reg(ops[0])] = ('addr', a) if a != idc.BADADDR else ('?', None)
            elif mn == 'STR' and len(ops) == 2 and reg(ops[0]) == 0 and pending:
                b, off = mem(ops[1])
                st = regs.get(b)
                if st and st[0] in ('got', 'addr') and st[1] is not None:
                    if st[0] == 'got':
                        v = ida_bytes.get_qword(st[1])
                        addr = v + base if v < base else v
                        slots.setdefault(pending, {'got': st[1], 'addr': addr, 'store': ea})
                    else:
                        slots.setdefault(pending, {'got': None, 'addr': st[1], 'store': ea})
                    last_ref = ea
                pending = None
            elif mn not in STORE_NO_DEST and ops:
                r = reg(ops[0])
                if r is not None and mn not in ('BL', 'BLR', 'BR', 'B') and not mn.startswith(COND_BR):
                    # a write to the register: forget it unless handled above
                    if r in regs and mn not in ('ADRP',):
                        pass
            ea = idc.next_head(ea, f.end_ea)
            if ea == idc.BADADDR:
                break
        if first_ref:
            loader_ranges.append((f.start_ea, f.end_ea))
    return slots, loader_ranges




# ---------------------------------------------------------------------------------------------------------------- uses
# Register state: ('base', V, delta) = the pointer V+delta (V is the address a GOT entry or ADRP/ADD gives);
#                 ('fn', name)       = the proc pointer of NVN function `name`.
# cells: address -> set of names; the memory cell at that address holds that proc's pointer. At first the cells are the slots found
# in the loader; a store of a proc pointer to a resolved global address adds a cell (a copy of the table), then the readers of
# that cell are scanned too.
class Uses:
    def __init__(self):
        self.calls = collections.defaultdict(set)      # name -> {ea of BLR/BR}
        self.loads = collections.defaultdict(set)      # name -> {ea of the instruction that reads the cell}
        self.copies = collections.defaultdict(set)     # name -> {ea of a store of the pointer into memory that is not the stack}
        self.unresolved = collections.defaultdict(set) # copies whose destination address could not be resolved
        self.passed = collections.defaultdict(set)     # name -> {ea of a BL with the pointer in x0..x7}
        self.new_cells = {}                            # addr -> set(names), found during this round


def clobber_call(regs):
    for r in list(regs):
        if isinstance(r, int) and r <= 18:
            del regs[r]


def parse_imm(t):
    m = re.match(r'^#?(-?)(0x[0-9a-fA-F]+|[0-9]+)$', t.strip())
    if not m:
        return None
    v = int(m.group(2), 0)
    return -v if m.group(1) else v


def explore(ea0, dest, state, cells, U, budget=BUDGET):
    work = [(ea0, {dest: state}, {})]
    seen = set()
    total = 0
    while work and total < budget * 6:
        ea, regs, stack = work.pop()
        regs, stack = dict(regs), dict(stack)
        steps = 0
        while steps < budget and ea != idc.BADADDR:
            steps += 1; total += 1
            key = (ea, tuple(sorted((k, v) for k, v in regs.items() if isinstance(k, int))), tuple(sorted(stack.items())))
            if key in seen:
                break
            seen.add(key)
            if not regs and not stack:
                break
            mn, ops = split_ops(line(ea))
            nxt = idc.next_head(ea, ea + 16)
            if mn in ('BLR', 'BR', 'BLRAA', 'BLRAAZ', 'BRAA', 'BRAAZ'):
                r = reg(ops[0]) if ops else None
                if r in regs and regs[r][0] == 'fn':
                    U.calls[regs[r][1]].add(ea)
                if mn.startswith('BLR'):
                    for r2 in range(8):
                        if r2 != r and r2 in regs and regs[r2][0] == 'fn':
                            U.passed[regs[r2][1]].add(ea)
                    clobber_call(regs)
                else:
                    break
            elif mn == 'BL':
                for r2 in range(8):
                    if r2 in regs and regs[r2][0] == 'fn':
                        U.passed[regs[r2][1]].add(ea)
                clobber_call(regs)
            elif mn == 'B':
                t = targets(ea)
                if not t:
                    break
                ea = t[0]
                continue
            elif mn in ('RET', 'RETAA', 'RETAB', 'ERET'):
                break
            elif mn.startswith('B.') or mn in ('CBZ', 'CBNZ', 'TBZ', 'TBNZ'):
                for t in targets(ea):
                    work.append((t, dict(regs), dict(stack)))
            elif mn == 'MOV' and len(ops) == 2:
                d, s0 = reg(ops[0]), reg(ops[1])
                if isinstance(d, int):
                    if s0 in regs:
                        regs[d] = regs[s0]
                    else:
                        regs.pop(d, None)
            elif mn == 'ORR' and len(ops) == 3 and ops[1].upper() in ('XZR', 'WZR') and isinstance(reg(ops[0]), int):
                d, s0 = reg(ops[0]), reg(ops[2])
                if s0 in regs:
                    regs[d] = regs[s0]
                else:
                    regs.pop(d, None)
            elif mn.startswith('CS') and len(ops) >= 3 and isinstance(reg(ops[0]), int):
                d = reg(ops[0])
                src = [regs[r] for r in (reg(ops[1]), reg(ops[2])) if r in regs]
                if src:
                    regs[d] = src[0]
                else:
                    regs.pop(d, None)
            elif mn == 'ADD' and len(ops) == 3 and isinstance(reg(ops[0]), int):
                d, s0 = reg(ops[0]), reg(ops[1])
                imm = parse_imm(ops[2])
                if s0 in regs and regs[s0][0] == 'base' and imm is not None:
                    regs[d] = ('base', regs[s0][1], regs[s0][2] + imm)
                else:
                    regs.pop(d, None)
            elif mn in ('LDR', 'LDUR') and len(ops) == 2 and isinstance(reg(ops[0]), int):
                d = reg(ops[0])
                b, off = mem(ops[1])
                st = regs.get(b)
                if b == 'sp' and ('sp', off) in stack:
                    regs[d] = stack[('sp', off)]
                elif st and st[0] == 'base' and isinstance(off, int) and (st[1] + st[2] + off) in cells:
                    names = sorted(cells[st[1] + st[2] + off])
                    for nm in names:
                        U.loads[nm].add(ea)
                    regs[d] = ('fn', names[0])
                else:
                    regs.pop(d, None)
            elif mn in ('STR', 'STUR') and len(ops) == 2:
                s0 = reg(ops[0])
                b, off = mem(ops[1])
                if s0 in regs:
                    if b == 'sp' and isinstance(off, int):
                        stack[('sp', off)] = regs[s0]
                    elif regs[s0][0] == 'fn':
                        nm = regs[s0][1]
                        U.copies[nm].add(ea)
                        st = regs.get(b)
                        if st and st[0] == 'base' and isinstance(off, int):
                            U.new_cells.setdefault(st[1] + st[2] + off, set()).add(nm)
                        else:
                            U.unresolved[nm].add(ea)
            elif mn == 'STP' and len(ops) == 3:
                b, off = mem(ops[2])
                for k, o in enumerate(ops[:2]):
                    r = reg(o)
                    if r in regs:
                        if b == 'sp' and isinstance(off, int):
                            stack[('sp', off + 8 * k)] = regs[r]
                        elif regs[r][0] == 'fn':
                            U.copies[regs[r][1]].add(ea)
                            U.unresolved[regs[r][1]].add(ea)
            elif mn == 'LDP' and len(ops) == 3:
                b, off = mem(ops[2])
                for k, o in enumerate(ops[:2]):
                    r = reg(o)
                    if isinstance(r, int):
                        if b == 'sp' and isinstance(off, int) and ('sp', off + 8 * k) in stack:
                            regs[r] = stack[('sp', off + 8 * k)]
                        else:
                            regs.pop(r, None)
            elif mn not in STORE_NO_DEST and ops and isinstance(reg(ops[0]), int):
                regs.pop(reg(ops[0]), None)
            ea = nxt


def got_entries(slots):
    """sorted [(target address, GOT entry address)] for the data segment(s) holding the slots' GOT entries."""
    base = get_image_base()
    segs = {}
    for d in slots.values():
        if d['got']:
            sg = ida_segment.getseg(d['got'])
            segs[sg.start_ea] = sg
    out = []
    for sg in segs.values():
        ea = sg.start_ea
        while ea + 8 <= sg.end_ea:
            v = ida_bytes.get_qword(ea)
            if v and v < base:
                out.append((v + base, ea))
            ea += 8
    out.sort()
    return out


def run_reads(addr_list, cells, U, got_idx, window, loader_ranges):
    """Explore every read of the cells: LDRs of GOT entries whose target lies in [cell - window, cell], and direct references."""
    import bisect
    keys = [v for v, _ in got_idx]
    done = set()
    for c in addr_list:
        lo = bisect.bisect_left(keys, c - window)
        hi = bisect.bisect_right(keys, c)
        for v, g in got_idx[lo:hi]:
            for x in idautils.XrefsTo(g):
                if any(a <= x.frm < b for a, b in loader_ranges):
                    continue
                mn, ops = split_ops(line(x.frm))
                if mn == 'LDR' and len(ops) == 2 and isinstance(reg(ops[0]), int) and (x.frm, v) not in done:
                    done.add((x.frm, v))
                    explore(idc.next_head(x.frm, x.frm + 16), reg(ops[0]), ('base', v, 0), cells, U)
        for x in idautils.XrefsTo(c):             # direct references to the cell itself
            if any(a <= x.frm < b for a, b in loader_ranges):
                continue
            mn, ops = split_ops(line(x.frm))
            if mn == 'LDR' and len(ops) == 2 and isinstance(reg(ops[0]), int) and (x.frm, c) not in done:
                done.add((x.frm, c))
                nm = sorted(cells[c])
                for n in nm:
                    U.loads[n].add(x.frm)
                explore(idc.next_head(x.frm, x.frm + 16), reg(ops[0]), ('fn', nm[0]), cells, U)


def find_import_slots():
    """Modules that import the proc pointers of the main module as data symbols (`pfnc_nvn<Name>`, e.g. the movie library in subsdk0):
    name -> {'got': [GOT entries naming the symbol], 'addr': the extern symbol address (stands for the variable)}."""
    out = {}
    for ea, nm in idautils.Names():
        if nm.startswith('pfnc_nvn') and NAME_RE.match(nm[5:]):
            gots = [x.frm for x in idautils.XrefsTo(ea) if idc.get_segm_name(x.frm) != '.text' and not ida_funcs.get_func(x.frm)]
            if gots:
                out[nm[5:]] = {'got': gots, 'addr': ea}
    return out


def analyze():
    slots, loader_ranges = find_slots()
    imports = {} if slots else find_import_slots()
    if imports:
        cells = {d['addr']: {n} for n, d in imports.items()}
        U = Uses()
        for n, d in imports.items():
            for g in d['got']:
                for x in idautils.XrefsTo(g):
                    mn, ops = split_ops(line(x.frm))
                    if mn == 'LDR' and len(ops) == 2 and isinstance(reg(ops[0]), int):
                        explore(idc.next_head(x.frm, x.frm + 16), reg(ops[0]), ('base', d['addr'], 0), cells, U)
        procs = {n: {'got': d['got'][0], 'addr': d['addr'], 'loads': sorted(U.loads[n]), 'calls': sorted(U.calls[n]), 'copies': sorted(U.copies[n]),
                     'unresolved_copies': sorted(U.unresolved[n]), 'passed': sorted(U.passed[n])} for n, d in imports.items()}
        return {'slots': len(imports), 'loader_ranges': [], 'procs': procs, 'copy_cells': {}, 'imports': True}
    cells = {}
    for name, d in slots.items():
        cells.setdefault(d['addr'], set()).add(name)
    U = Uses()
    got_idx = got_entries(slots)
    run_reads(list(cells), cells, U, got_idx, 0, loader_ranges)
    copy_cells = {}
    for rnd in range(ROUNDS):
        fresh = {a: n for a, n in U.new_cells.items() if a not in cells}
        U.new_cells = {}
        if not fresh:
            break
        for a, n in fresh.items():
            cells[a] = set(n)
            copy_cells[a] = set(n)
        run_reads(list(fresh), cells, U, got_idx, 0x4000, loader_ranges)
    procs = {}
    for name, d in slots.items():
        procs[name] = {'got': d['got'], 'addr': d['addr'], 'loads': sorted(U.loads[name]), 'calls': sorted(U.calls[name]),
                       'copies': sorted(U.copies[name]), 'unresolved_copies': sorted(U.unresolved[name]), 'passed': sorted(U.passed[name])}
    return {'slots': len(slots), 'loader_ranges': loader_ranges, 'procs': procs,
            'copy_cells': {hex(a): sorted(n) for a, n in copy_cells.items()}}
