# nx_extract

Turns the player's own dump files into the extracted layout the launcher, the lifter and the runtime read, and converts ZBIC NSOs to
plain ones. Game independent: the game's program id comes from the command line (the launcher passes `[game] title_id` of game.toml).

```text
nx_extract --input base.xci --input update.nsp --keys prod.keys[,title.keys] --title-id 0400A3A01EE4C000 --out D:\Game\dump
nx_extract --input base.nsp ... --check          # open and identify only, write nothing
nx_extract --plain-nso <exefs folder> <out folder>
```

Output: `<out>/base/exefs/`, and either `<out>/updates/<version>/{exefs,romfs}` (romfs = the base RomFS with the update applied) or
`<out>/base/romfs/` when no update is given, then `<out>/extract.json`. A second run keeps files whose size already matches.
The dump's own hash trees are checked first (`--no-verify` skips that). Launcher protocol on stdout: `@progress <phase> <done>
<total> <text>`, `@note`, `@error`, `@result <json>`; exit codes 0 done, 1 error, 2 bad dump, 3 keys, 4 disk, 5 stopped
(`--cancel-file`).

Inputs: NSP and XCI; NSZ and XCZ when built after `tools\setup\scripts\fetch_zstd.ps1`. Keys: the player's `prod.keys` (header key or
its sources, master keys, key area key and titlekek sources) and, for eShop dumps whose ticket is not in the NSP, `title.keys`.
Values stay in memory; nothing logs or writes them.

The readers are tools/setup/src (container.cpp, nca.cpp, bktr.cpp, keys.cpp, ...: written for the NX setup from the switchbrew pages,
checked there against a real Switch 1 dump, including the newer patch-NCA layout that stops hactool with "Invalid BKTR layout").
ZBIC: runtime/src/loader/zbic.cpp, format in runtime/docs/zbic.md.

Build and test: `pwsh -File tools\extract\scripts\build.ps1 -Test` (CMake, Ninja, clang-cl; `build\release\bin\nx_extract.exe`).
The test builds synthetic NSP and XCI files with fake keys (tools/setup/tests builders). No real Switch 2 container was available.
