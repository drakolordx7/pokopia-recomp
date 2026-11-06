zstd 1.5.7 single-file decompressor with ZBIC support (decoder only), BSD-3-Clause (zstd's dual licence, BSD option; LICENSE here).

Source: https://github.com/hexkyz/zstd, commit 02a8f10c6f4acfcfba2e31e034fbae9f07938f96 ("ZBIC improvements", 2026-04-02), a fork of
facebook/zstd dev at f8745da6ff (2025-02-19, = v1.5.7) with two commits by hexkyz that add ZBIC (lib/common/entropy_common.c,
lib/common/fse.h, lib/zstd.h and the compressor side). The same code is what Atmosphere-NX (libstratosphere util/zstd.inc), kinnay/zbic
and alula/beetool ship; the BIC table and FSE_readNCount_bic were compared line by line with kinnay/zbic 11b08f2 and are identical.
GitHub tarball of that commit: sha256 83b86ce66931a6e815d7195422b8384e78983968bdf4313eb60e5cdf195354ce.

Generated, unchanged, with zstd's own amalgamation script:
  cd build/single_file_libs && python combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c
zstddeclib.c sha256 2f67a35f2187204d923723f322b3aadce5e6f02080987cd2db993a3df75c1474.

Built only through src/loader/zbic.cpp, which defines ZSTD_ZBIC_SUPPORT 1 and gives every symbol internal linkage. Format notes:
docs/zbic.md.
