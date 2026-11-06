# NVN2 shader layout: cross-check of our control section and SPH parsing

Our reading of the NVN2 (Switch 2) shader control section and shader program header (SPH) was compared field by field with two
independent sources, and every field was tested against the corpora.

| Source | Licence | What it says about the layout |
|---|---|---|
| **ours** | | `tools/shaders/docs/AMPERE.md` sections 2-4, `runtime/src/gfx/nvn/nvn_shaders.{h,cpp}` (`read_control`, `apply_nvn2_compute_table`), `tools/shaders/harness` (`Program.cs`, `StaticGpuAccessor.cs`, `Nvn2Reflection.cs`) |
| **dkb** = [dkb-pc-recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp) (`refs/dkb-pc-recomp`) | GPL-3.0 | `docs/bananza-shaders.md` ("The NVN 2 control section" table), `runtime/gfx_shader.c` (`nvn2_sizes`, `gfx_shader_nvn2_hash`), `runtime/nvn2_lower.cpp`, `runtime/nvn2_c0.h`, `tools/dkb_nvn2_{ctl,sph}stats.py` |
| **Eglantine** = [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine) branch `nx2` (`refs/Eglantine`) | GPL-3.0 | `NX2_NOTES.md` is a log of GPU-emulation fixes and says nothing about the control section; its code (`shader_environment.cpp`, `shader_recompiler/program_header.h`, `frontend/ampere/*`) shows how it takes the SPH |
| Mesa NAK `sph.rs` (MIT) and NVIDIA open-gpu-doc `clc797sph.h` | | the SPH v4 bit positions (NAK is the only one of the three that writes a v4 header) |

Nothing was copied from the GPL sources; facts only (offsets and meanings), and every verdict below is backed by a measurement on the corpora, not
by agreement between the sources.

**Corpora**: Pokopia (`pkp`, 99,012 distinct code blocks, GLSLC outputs from the game's files), Donkey Kong Bananza (`dkb`, 2,151 blocks cut from
the BNSH in its `.bfsha` archives) and Mario Kart World (`mkw`, 5,705 blocks dumped from `nvnProgramSetShaders` at run time). Stage counts:
pkp 90,917 ps / 6,123 vs / 1,386 cs / 527 tes / 50 tcs / 9 gs; dkb 1,587 ps / 561 vs / 3 gs; mkw 2,786 ps / 2,693 vs / 207 cs / 7 + 12 blocks
whose stage is 6 or 7. Reproduce: `xcheck.exe --layout` (`tools/shaders/xcheck/README.md`); it writes `layout_words.tsv` (value distribution of every
control word per corpus and stage), `layout_invariants.tsv`, `layout_crosstabs.tsv`, `sph_words.tsv`, `sph_io.tsv` to `work/xcheck/`.
The Bananza blocks come from BNSH, ours from GLSLC outputs, MKW from the runtime: all three have the same control layout, so the section is the same
whichever way it is reached.

## 1. Control section (0xd8 bytes, plus a tail in 8% of blocks)

Verdicts: **agree** (all sources and the corpora), **ours right**, **dkb right**, **fixed** (we were wrong, fixed in this branch), **new** (a field none of the
sources named, found by the statistics), **n/a** (the source does not cover it).

| Offset | Ours | dkb | Eglantine | Verdict | Evidence |
|---|---|---|---|---|---|
| +0x00 | magic `0x98761234` | same | n/a | agree | 106,868 of 106,868 blocks |
| +0x04 | `0xc9` (201), *inferred* version | `0xc9`, NVN 1 has 1 | n/a | agree | all blocks; `read_control` and `gfx_shader_is_nvn2` both test it |
| +0x08 | `0xb` / `0x11` / `0x4`, not the stage, meaning unknown | `0xb` (2,626) or `0x7` (30), not the stage | n/a | agree on "not the stage"; values differ | pkp: 0xb 95,096, 0x11 3,907, 0x4 9 (any stage); dkb and mkw: 0xb only. dkb's `0x7` is not in our 2,151 Bananza blocks (their corpus has 2,656 stages); the field is a compiler flavour, not read by anyone |
| +0x0c | `0x000e0170` | same, "target, probably SM86" | n/a | agree | constant in all 106,868 blocks |
| +0x18 | u16 `0x15`, u16 `0x22` = GLSLC version | n/a | n/a | ours | 1 pkp block has `0x220615`; the rest `0x220015` |
| +0x1c | `0x00288000` | n/a | n/a | ours | constant |
| +0x20..+0x2f | 16-byte hash; the upper 8 bytes are the same for every copy of a code block | 128-bit hash | n/a | agree | unique per block: +0x20 has 6,121 distinct values over 6,123 vs blocks, +0x28 and +0x2c 6,123 |
| +0x30 | control length (0xd8 base) | control length, 0 means 0xd8 (182 Bananza stages) | n/a | agree; the zero case is run-time only | pkp and dkb: +0x30 equals the control file length in every block (0xd8, 0xe0, 0xf8, 0x108, 0x118 ...). mkw (run-time capture): 278 blocks have 0, and then +0x98, +0xbc, +0xc4, +0xc8 are 0 too; `read_control` uses `max(0xd8, +0x30)` like dkb's `nvn2_sizes` |
| +0x38 | program length (SPH + instructions), multiple of 0x80, <= +0x40 | "code size" | n/a | agree | multiple of 0x80, <= code file length, <= +0x40 in all blocks |
| +0x3c | constant data (c[1]) length | not read ("constants from +0x40 to +0x44") | n/a | ours right | dkb copies `[+0x40, +0x44)` as c[1]. +0x44 - +0x40 equals +0x3c in only 8,622 of 69,341 pkp blocks with constants (0 of 13 dkb, 0 of 49 mkw); the rest is zero padding up to +0x44 (checked on a 1-in-40 sample: 1,747 of 1,747 gaps are all zero bytes). dkb over-reads zeros, which is harmless to it; +0x3c is the exact length |
| +0x40 | constant data offset in the code area, multiple of 0x100 | start of c[1] | n/a | agree | 69,341 of 69,341 (pkp), 13 (dkb), 49 (mkw) with constants are 0x100-aligned; +0x40 + +0x3c <= +0x44 in all |
| +0x44 | code area length (hash / upload size) | end of c[1], and the code size used for the hash | n/a | ours right (exact meaning) | equals the code file length in all 106,868 blocks; GLSLC code header "code length" for pkp. dkb's `nvn2_sizes` takes `max(+0x38, +0x44)` as the length, the same thing |
| +0x4c | register count, *inferred* | "small (0x24..0x48), likely the register count" | n/a | agree on the meaning; dkb's range is a Bananza sample | pkp range is 4..255; 0x24..0x48 holds for 25,356 of 99,012 pkp blocks only. `4` is the smallest, 255 the largest value, 25 blocks fall outside 6..0xff (values 4 and 5) |
| **+0x50** | ~~shared memory bytes~~ **local memory bytes per thread** | not read | n/a | **fixed** (was wrong; dkb never read it) | non-zero exactly when the SASS has LDL/STL: pkp cs 571 of 571 blocks with LDL/STL have it and 815 without do not; pkp ps 921 of 921 / 3,624 of 3,624; dkb vs 3 of 3; mkw ps 1 of 1. For graphics it coincides with SPH word 1 `shader_local_memory_low_size` != 0 (921 pkp ps, 3 dkb vs, 1 mkw ps; never otherwise). Values 0x200, 0x400, 0xc00, 0x1800 ... |
| **+0x54** | not named | "multiple of 0x1000" | n/a | **new**: `stage << 16`, low half is flags | vs 0, ps 0x10000, gs 0x20000 (0x20001 in 2), tcs 0x30000, tes 0x40000, cs 0x50000, each constant per stage; u16 +0x56 below is its upper half |
| +0x56 | u16 stage 0 vs, 1 ps, 2 gs, 3 tcs, 4 tes, 5 cs | no stage field: dkb takes the stage from SPH word 0 and says "+0x08 is not the stage" | task / mesh stages as vertex / tessellation SPH shader types (row 89) | ours right; extended: **6 and 7 exist** | agrees with the SPH shader type of every graphics block with stage 0..4 (pkp 97,626 of 97,626, dkb 2,151 of 2,151, mkw 5,479 of 5,498). The 19 mkw exceptions are stage 6 (7 blocks, SPH shader type 1) and stage 7 (12 blocks, SPH shader type 3 for 7 and 1 for 5): task and mesh shaders, which Eglantine maps to the vertex and tessellation slots (a mesh shader paired with a task shader uses the tessellation type, as in NAK `ShaderType::from`). Not present in pkp or dkb; our harness rejects stage > 5 with a clear message |
| +0x58/+0x5c/+0x60 (cs) | work group x, y, z | copied as the local size (`nvn2_lower.cpp` memcpy of 12 bytes from +0x58) | n/a | agree | x*y*z is within 1..1024 in 1,386 of 1,386 pkp and 207 of 207 mkw compute blocks; z is 1, 4 or 8 |
| +0x58/+0x5c/+0x64 (tes) | GL enums: domain, spacing, winding | n/a | n/a | ours (single source) | 527 of 527 tes: 4 / 0x8e7b / 0x901; gs: +0x58 = input primitive (0 points, 4 triangles); tcs 4 / 0x202 / 0x901 in 50 of 50 |
| +0x58/+0x5c (ps) | not named | n/a | n/a | **new** | +0x5c is 0x01000000 exactly when SPH `omap_depth` (bit 1 of word 19) is set (1,697 of 1,697, none otherwise, sample of every 7th ps block); +0x58 bit 0 is set with it in all but 5 of those; other values: +0x58 bit 8 (50 blocks) unknown |
| +0x60 (ps) | not named ("multiple of 0x1000" in dkb) | n/a | n/a | **new**: number of colour render targets << 16 | equals (highest RT written in SPH `omap_target` + 1) << 16 in 12,989 of 12,989 sampled ps blocks |
| **+0x64** (cs) | ~~tes winding only~~ **shared memory bytes** | compute shared memory size | n/a | **fixed** (dkb right) | non-zero exactly when the SASS has LDS/STS/ATOMS (pkp cs 1,035 with and 351 without; mkw 14 with, 188 without; the 5 other mkw blocks use only REDUX, which is not shared memory); every static LDS/STS/ATOMS address of the 95 pkp and 3 mkw blocks that have one ends within +0x64, while it ends within +0x50 in only 1 of 95. mkw: +0x50 is 0 for all 207 compute blocks and +0x64 is not (14 blocks): the old reading lost the shared array entirely. Values 0x400, 0xb00, 0xf00, 0xbc00 ... (tes/tcs: GL winding, above) |
| +0x68 | +0x50 / 32 (*inferred*) | n/a | n/a | ours, compute only | holds for all 1,386 pkp and 207 mkw compute blocks; for ps +0x68 is 0 whatever +0x50 is (18,472 of 18,472 ps with local memory), so it is not a general rule. A derived local-memory field of the QMD |
| +0x98 | = control length | "repeated at +0x98, +0xc4, +0xc8" | n/a | agree | +0x98 == +0x30 in every block with +0x30 != 0 |
| +0xa4 | `0x01000000` (graphics) | `0x1000000`, constant | n/a | agree | all graphics blocks; compute has 0x1010000 in 102 pkp / 22 mkw blocks |
| +0xa8..+0xb8 | compute-only words | +0xa9..+0xb0 root table word counts, +0xb1 uniform buffers, +0xb2 storage buffers, +0xb3 textures, +0xb6 images | n/a | agree on the counts; ours adds the +0xba check | bytes +0xb1/+0xb2/+0xb3/+0xb6 are what `apply_nvn2_compute_table` reads; u16 +0xba == 0x10 + 0x10 * (+0xb1 + +0xb2) + 8 * (+0xb3 + +0xb6) in 1,386 of 1,386 pkp and 207 of 207 mkw compute blocks (dkb's `nvn2_c0.h` computes the same sizes); bytes +0xa9..+0xb0 are the eight root table word counts (the word at +0xb0 alone takes 101 distinct values in the pkp compute blocks) |
| +0xbc/+0xc4/+0xc8 | control length, 0xe0 when a tail exists | "+0x30, repeated at +0xc4, +0xc8" | n/a | agree, with an exception | equals `+0x30 > 0xd8 ? 0xe0 : +0x30` in 99,010 of 99,012 pkp blocks; the two others are gs blocks whose +0xc8 is the control length (0xf8, 0x100); mkw: 2 more with the control sizes 0x338 / 0x930 |
| +0xd8 tail | u32 table offset 0xe0, length; table of {code offset, word count, 0x00010000, running total} (*inferred* patch points) | n/a | n/a | ours (single source) | present when +0x30 > 0xd8: 8% of pkp blocks; unchanged |
| control size 0x930 / 0x338 | n/a | "0x930 in sead" | n/a | agree | mkw has one vs with +0x30 = 0x930 and one ps with 0x338 (sead-style built-in programs); the rest of the section keeps the NVN2 layout |
| c[24] | `ResourceManager` reserves bank 24 | c24 is read only by full-screen blit shaders | row 65: blit pass reads bank 24 | agree | not part of the control section; listed because both sources describe it |

Everything else in the first 0x100 bytes is constant (0) in all corpora except the compute-only words at +0xa0 (0x1000000 or 0), +0xa8 (1),
+0xb0..+0xb8 and the tail at +0xdc.

### Bugs found and fixed

1. **Shared and local memory sizes were swapped** (`+0x50` read as the shared memory size; shared memory is `+0x64`). Affected the harness
   (`StaticGpuAccessor.QueryComputeSharedMemorySize`: compute shaders with shared memory but no local memory were translated with the
   translator's 4 KB default array, shaders with local memory only got an array the size of the local memory) and `read_control`
   (`ShaderControl::shared_size`, not consumed by anything yet). Fixed in commit `6495f54`; the SPIR-V of the affected compute shaders changes
   (any translated-shader cache keyed on the old output has to be rebuilt).
2. Documentation only: `AMPERE.md` listed +0x50 as shared memory and +0x64 as tes-only; it now has both.

Not bugs but gaps worth knowing: the harness does not pass the local memory size (+0x50) to `QueryComputeLocalMemorySize` (the translator's default is used);
stage 6/7 (task, mesh) blocks of Mario Kart World are rejected by the harness.

## 2. Shader program header (SPH, graphics stages only)

All sources agree on the structure: graphics code starts with a 0x80-byte SPH v4 (32 words), instructions follow at +0x80, compute has none.
Eglantine reads the first 0x50 bytes with the Maxwell (version 3) `ProgramHeader` and starts code at 0x80 (`source_header_size = 128`, 0 for compute);
dkb copies the first 0x50 bytes into a Maxwell header (`nvn2_lower.cpp`); our translator reads the same first 0x50 bytes through Ryujinx's
`ShaderHeader` (nxnative uses the identical reader). The v4 additions (words 20..31) are not read by anyone.

| Field (bits of the 32-word header) | Ours | dkb | Eglantine / NAK | Verdict | Evidence |
|---|---|---|---|---|---|
| word 0 bits 0-4 `sph_type` | 1 VTG, 2 PS | 1 VTG, 2 PS | `sph_type` | agree | ps always 2, others always 1: pkp 97,626 of 97,626, dkb 2,151, mkw 5,498 |
| word 0 bits 5-9 `version` | 4 | 4 | NAK: 4 when SM >= 73 | agree | 4 in every graphics block |
| word 0 bits 10-13 `shader_type` | 1 vs, 2 tcs, 3 tes, 4 gs, 5 ps | 1 vertex, 5 pixel | NAK `SPHV3_T1_SHADER_TYPE_*`: same values | agree | equals the control stage in 97,626 of 97,626 pkp blocks; mkw task/mesh: see +0x56 |
| word 0 bits 14, 15, 16 `mrt_enable`, `kills_pixels`, `does_global_store` | read by the translator (`ShaderHeader`) | n/a | Maxwell layout | agree (layout) | pkp ps: mrt_enable 90,917 of 90,917; kills_pixels 60,556 of 90,917; does_global_store 4 ps and 24 vs |
| word 0 bits 17-20 `sass_version` | read, not used | n/a | `SASS_VERSION` | agree | 6 in every block of all three games (Ampere SASS) |
| word 0 bit 24 / 25 | gs passthrough / (vs) isbe space sharing | n/a | NAK: bit 24 `gs_passthrough_enable`, bit 25 `isbe_space_sharing_enable` | agree | bit 24 is 0 in all pkp blocks; bit 25 is 1 in 4,483 of 6,123 pkp vs |
| word 0 bits 26, 27, 28-31 | `does_load_or_store`, `does_fp64`, `stream_out_mask` | n/a | same | agree | does_fp64 and does_load_or_store always 0; stream_out_mask 1 in 9 of 9 gs |
| word 1 bits 24-31 `per_patch_attribute_count` | read | n/a | NAK sets it for the tessellation-init stage | agree | non-zero only for tcs (50 of 50) |
| word 2 bits 24-31 `threads_per_input_primitive` | read | n/a | same | agree | 3 for all tcs, 1 for all gs |
| word 3 bits 24-27 `output_topology`, word 4 bits 0-11 `max_output_vertices` | read | n/a | same | agree | gs: topology 7 (triangle strip) in 6, 1 (points) in 3 |
| words 1-3 low 24 bits, local memory sizes | read (local memory size) | n/a | `shader_local_memory_{low,high,crs}_size` | agree | non-zero (word 1) exactly when the control +0x50 and LDL/STL are, see above |
| words 6-13 (bits 192..447), ps `imap_generic_vector` | 2 bits per component: 0 unused, 1 constant, 2 perspective, 3 screen-linear | n/a | NAK `imap_g_ps` 192..448 | agree | every generic `IPA` read in the sampled blocks (pkp 144,028 reads in 4,545 ps blocks, dkb 37,844 in 1,587, mkw 62,981 in 2,786) reads a component whose imap is non-zero, and `IPA.CONSTANT` always has imap 1. `IPA.PASS` has imap 2 in all 144,028 pkp reads; in dkb 3,828 and in mkw 8 `IPA.PASS` reads have imap 3 (see the note) |
| bits 432..559 (words 13-17), vs `omap_generic_vector` | 4 bits per vector, one per component | n/a | NAK `omap_g` 432..560 | agree | every generic AST component of the sampled vertex shaders (pkp 137,391 in 8,095 blocks, dkb 13,179, mkw 73,397) has its omap bit set: 0 violations |
| word 18 (bits 576..607), ps `omap_target` | 4 bits per render target | n/a | NAK `omap_target` 576..608 | agree | e.g. 0xf (3,179 of 3,637 sampled blocks), 0xffffff, 0xfffffff, 0; equals the control +0x60 >> 16 render target count (12,989 of 12,989) |
| word 19 bits 0, 1 `omap_sample_mask`, `omap_depth` | read | n/a | `SPHV3_T2_OMAP_*` | agree | bit 1 set in 431 of 3,637; bit 0 never; bit 1 coincides with control +0x5c = 0x01000000 |
| bytes 0x50..0x7f (words 20-31, the v4 extension) | not read | not copied | not read (Eglantine); NAK sets `reads_interlock_ticket`, `uses_inner_coverage`, `pervertex_imap` here | agree | all zero in every graphics block of all three corpora (105,275 of 105,275 graphics blocks): none of the v4-only features is used by these games |

Note on imap 3: `IPA.PASS` serves both perspective (imap 2) and screen-linear (imap 3, `noperspective`) inputs; the instruction cannot tell them apart and the
header does (our front end multiplies a Perspective input by 1/w after Ryujinx's IPA.PASS, see `EmitIpaAmpere`). Pokopia has no `noperspective`
input in the sampled blocks, Bananza and Mario Kart World do, so the 3,828 and 8 reads are expected and the check above is consistent with the
header layout (a wrong bit position would have produced reads of unused components).

## 3. What was not comparable

* **GLSLC output header and the G2S container** (`AMPERE.md` sections 1 and 2): neither dkb nor Eglantine reads GLSLC outputs. dkb takes its
  shaders from BNSH inside `.bfsha` (the 2,151 Bananza blocks of the `dkb` corpus were cut from BNSH with `extract_bnsh_corpus.py`, and all control
  invariants hold for them), Eglantine from GPU memory. The Bananza blocks do show that the control section inside a BNSH equals the one in a GLSLC
  output.
* **Reflection section (type 3)**: not parsed by us beyond `Nvn2Reflection.cs`; dkb reads reflection from its own container.
* dkb's task/mesh and ASTC notes, and Eglantine's lowering to Maxwell, are outside this check.
