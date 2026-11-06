// SPDX-License-Identifier: GPL-3.0-or-later
// nx_extract: turns the player's raw dump files (NSP / XCI, NSZ / XCZ when built with zstd; base game plus an update) and their
// keys into the extracted-dump layout the launcher and the lifter read:
//   <out>/base/exefs/                    the base game's ExeFS (always)
//   <out>/base/romfs/                    the base game's RomFS (only when no update is given)
//   <out>/updates/<version>/exefs/       the newest update's ExeFS
//   <out>/updates/<version>/romfs/       the RomFS with the update applied (the whole game, not only the changed files)
//   <out>/extract.json                   what was extracted from which inputs (written last; its presence means complete)
// and converts ZBIC NSOs (runtime/docs/zbic.md) to plain NSOs. The container, NCA, key and update code is tools/setup/src (written
// for the NX setup, checked there against a real Switch 1 dump); this file only drives it. Key values are never logged or written.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common.h"

namespace mprs {

struct ExtractOptions {
    std::vector<std::string> inputs;       // dump files, any order
    std::vector<std::string> key_files;    // prod.keys, optional title.keys
    std::string title_id;                  // 16 hex digits, the game's program id
    std::string out_dir;
    bool verify = true;                    // check the dump's own hash trees before writing
    int threads = 0;                       // 0 = up to 8
    std::function<bool()> cancelled;       // polled between chunks
    // phase: "open", "verify", "exefs", "romfs"
    std::function<void(const char* phase, u64 done, u64 total, const std::string& message)> progress;
};

struct ExtractResult {
    std::string dir;                       // "base" or "updates/<version>": the folder the launcher should use
    std::string build_id;                  // main NSO build id, 40 hex digits
    u32 version = 0;                       // the update's title version, 0 for the base game
    std::string display_version;           // from control.nacp, "" if unknown
    u64 files = 0, bytes = 0;              // RomFS files and bytes written or kept
    int zbic_modules = 0;                  // NSOs of the chosen ExeFS that use ZBIC (kept as dumped)
    std::vector<std::string> notes;        // things the player should know (an input not used, an older update skipped)
};

// Throws Error (kind Input / Keys / Disk / Cancelled) with a message for the player. Resumable: files already present with the right
// size are kept. Inputs and outputs only; keys are read from key_files and stay in memory.
ExtractResult extract_dump(const ExtractOptions& o);

// Opens the inputs and keys and says what would be extracted, writing nothing (the launcher's check before Set up).
ExtractResult inspect_dump(const ExtractOptions& o);

// True when the NSO at path uses ZBIC (flag bit 7).
bool nso_is_zbic(const std::string& path);
// Writes `out` as a plain NSO with the same contents as `in`: header kept, segments stored uncompressed (compressed sizes = sizes,
// compression flags and the ZBIC flag cleared, the segment hash flags and hashes kept). LZ4 and ZBIC inputs both work. Throws
// Error(Input) on a damaged NSO (a segment that does not decompress to its size or fails its hash when the hash flag is set).
void nso_to_plain(const std::string& in, const std::string& out);
// Copies every file of the ExeFS folder in_dir to out_dir, converting ZBIC NSOs with nso_to_plain. Returns how many were converted.
int exefs_to_plain(const std::string& in_dir, const std::string& out_dir);

}  // namespace mprs
