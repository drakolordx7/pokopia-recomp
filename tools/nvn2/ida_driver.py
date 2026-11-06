"""Run by `idat -A -S<this file> <database>` (see nvn_use.py): waits for auto-analysis, runs ida_nvn_scan.analyze() and writes JSON.
Environment: NVN2_SCAN_OUT = output JSON path. The database is not saved (DBFL_KILL) unless NVN2_SCAN_KEEP=1."""
import os, sys, json, traceback
import ida_auto, ida_pro, ida_loader

out = os.environ['NVN2_SCAN_OUT']
try:
    ida_auto.auto_wait()
    scan_dir = os.environ['NVN2_SCAN_DIR']
    ns = {'__name__': 'nvn_scan'}
    with open(os.path.join(scan_dir, 'ida_nvn_scan.py'), encoding='utf-8') as f:
        exec(compile(f.read(), 'ida_nvn_scan.py', 'exec'), ns)
    res = ns['analyze']()
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(res, f)
except BaseException:
    with open(out + '.err', 'w', encoding='utf-8') as f:
        f.write(traceback.format_exc())
if os.environ.get('NVN2_SCAN_KEEP') != '1':
    ida_loader.set_database_flag(ida_loader.DBFL_KILL)     # analysed copies are not saved; a database made from an ELF is (NVN2_SCAN_KEEP=1)
ida_pro.qexit(0)
