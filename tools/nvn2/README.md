# NVN2 function use map

Which NVN2 functions a game's code actually calls (not only looks up by name), and whether our runtime implements them.

```
toolchain\.venv\Scripts\python.exe -I tools\nvn2\nvn_use.py --game <name> --module main=<path> [--module subsdk2=<path> ...]
```

`<path>` is an IDA database (`.i64`, a copy is analysed, the original is never opened), an ELF, or an NSO from the exefs (converted with
`toolchain\nx2elf` into `work\ida\<game>\`, then analysed headless by IDA Pro 9.4 `idat`, `IDA_DIR` overrides the install folder;
the database stays in `work\ida\<game>\` and later runs reuse it). Analysing a big NSO takes many minutes; an existing database takes seconds.
Use the update's exefs when a game has one.

Output (default `<repo root>\work\audit\`):

- `nvn_use_<game>.tsv`: `function`, `called` (yes/no), `call_sites`, `implemented` (yes/stub), `group`, `looked_up`, `read_sites`, `copies`, `modules`.
- `nvn_use_<game>_summary.md`: counts, the stubbed functions the game calls by feature group (ray tracing, mesh, DLSS, device-generated
  commands, VRS, push constants, ProgramBuilder, dirty tile map, message buffer, other), stubs looked up but never called.

How it works (`ida_nvn_scan.py`, run inside IDA by `ida_driver.py`):

1. The loader is the function that references the `nvn...` name strings. After each `BLR` (the `nvnDeviceGetProcAddress` call) the next
   `STR X0,[Xn]` stores the pointer; `Xn` resolves through ADRP/LDR (GOT) to the global that holds it. Result: name -> global.
2. Every read of such a global (xrefs to its GOT entry) starts a forward dataflow over the control-flow graph (moves, selects, stack spills
   and reloads, address arithmetic, 90 instructions per path). `BLR`/`BR` on the pointer = call site (distinct instruction addresses).
3. A store of the pointer into other memory is a copy of the table: when the destination resolves to a global, that cell's readers are
   scanned too (3 rounds); destinations it cannot resolve are listed in the summary ("Could not follow"). Pointers handed to a function in
   x0..x7 are counted in `passed` (kept in the raw JSON, `--keep-json`).

"Implemented" is read from the runtime source: names registered with `NVN_REG(...)` / `NVN2_REG(...)` under `runtime/src` plus
`nvnBootstrapLoader` / `nvnDeviceGetProcAddress`; the proc list is `runtime/src/gfx/nvn/nvn2_procs.tsv`. Implementations that only record or
ignore a call count as implemented. Pass `--runtime <dir>` to read another runtime tree (default: `runtime` next to `tools`).

Modules without a loader that import the pointers as `pfnc_nvn<Name>` data symbols (the movie library in Pokopia's subsdk0) are read through
their GOT entries instead. Check which modules touch NVN first: `nvn[A-Z]...` strings in the uncompressed NSO (`nx2elf <nso> --export-uncompressed <file>`).

Limits: calls that go through a pointer passed into another function, or through a table the game builds in heap memory, are not followed.
