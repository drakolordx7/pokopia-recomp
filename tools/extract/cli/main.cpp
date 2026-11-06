// SPDX-License-Identifier: GPL-3.0-or-later
// nx_extract: raw dump files + keys -> the extracted-dump layout (src/extract.h), and ZBIC NSOs -> plain NSOs.
// Machine-readable output for the launcher on stdout: "@progress <phase> <done> <total> <text>", "@note <text>", "@error <text>",
// "@result <json>". Exit codes: 0 done, 1 error, 2 the dump cannot be used, 3 keys, 4 disk, 5 stopped (run again to continue).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "extract.h"
#include "fileio.h"
#include "log.h"

using namespace mprs;

namespace {

std::mutex g_out;

void line(const std::string& s) {
    std::lock_guard lk(g_out);
    std::fputs(s.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string one_line(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string jesc(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) r += c;
    }
    return r;
}

void split_into(std::vector<std::string>& v, const std::string& s) {
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
}

int exit_code(Fail k) {
    switch (k) {
    case Fail::Input: return 2;
    case Fail::Keys: return 3;
    case Fail::Disk: return 4;
    case Fail::Cancelled: return 5;
    default: return 1;
    }
}

void usage() {
    std::puts(
        "Usage:\n"
        "  nx_extract --input <dump> [--input <update>] --keys <prod.keys>[,<title.keys>] --title-id <16 hex> --out <folder>\n"
        "             [--check] [--no-verify] [--threads N] [--cancel-file <path>] [--log <file>]\n"
        "  nx_extract --plain-nso <exefs folder> <out folder>\n"
        "\n"
        "  --input        NSP / XCI (NSZ / XCZ when built with zstd): the base game and its update, any order (repeat or comma list)\n"
        "  --keys         the player's prod.keys and optionally title.keys (read only, never copied or logged)\n"
        "  --title-id     the game's program id\n"
        "  --out          writes <out>/base/exefs, and <out>/updates/<version>/{exefs,romfs} or <out>/base/romfs, then extract.json\n"
        "  --check        open the dump and print what it holds, write nothing\n"
        "  --no-verify    skip the dump's hash check\n"
        "  --cancel-file  stop (exit 5) once this file exists\n"
        "  --plain-nso    copy an ExeFS folder, converting ZBIC-compressed NSOs to plain ones\n"
        "\n"
        "Exit codes: 0 done, 1 error, 2 the dump cannot be used, 3 keys, 4 disk, 5 stopped.");
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow(wargv[i]));
    ExtractOptions o;
    bool check = false;
    std::string cancel_file, log_file;
    try {
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto val = [&]() -> const std::string& {
                if (i + 1 >= args.size()) fail(Fail::Generic, "%s needs a value.", a.c_str());
                return args[++i];
            };
            if (a == "--input") split_into(o.inputs, val());
            else if (a == "--keys") split_into(o.key_files, val());
            else if (a == "--title-id") o.title_id = val();
            else if (a == "--out") o.out_dir = val();
            else if (a == "--check") check = true;
            else if (a == "--no-verify") o.verify = false;
            else if (a == "--threads") o.threads = std::stoi(val());
            else if (a == "--cancel-file") cancel_file = val();
            else if (a == "--log") log_file = val();
            else if (a == "--plain-nso") {
                if (i + 2 >= args.size()) fail(Fail::Generic, "--plain-nso needs an ExeFS folder and an output folder.");
                const int n = exefs_to_plain(args[i + 1], args[i + 2]);
                line("@result {\"converted\": " + std::to_string(n) + "}");
                return 0;
            } else if (a == "-h" || a == "--help") {
                usage();
                return 0;
            } else fail(Fail::Generic, "Unknown option %s (see --help).", a.c_str());
        }
        if (o.title_id.empty()) fail(Fail::Generic, "--title-id is required.");
        if (!log_file.empty()) log_open(log_file);
        log_set_sink([](LogLevel l, const std::string& m) { if (l >= LogLevel::Warn) line("@note " + one_line(m)); }, LogLevel::Warn);
        std::atomic<bool> cancel{false};
        if (!cancel_file.empty()) o.cancelled = [&] {
            if (!cancel && path_exists(cancel_file)) cancel = true;
            return cancel.load();
        };
        o.progress = [](const char* phase, u64 done, u64 total, const std::string& msg) {
            line("@progress " + std::string(phase) + " " + std::to_string(done) + " " + std::to_string(total) + " " + one_line(msg));
        };
        const ExtractResult r = check ? inspect_dump(o) : extract_dump(o);
        for (const std::string& n : r.notes) line("@note " + one_line(n));
        line("@result {\"dir\": \"" + r.dir + "\", \"version\": " + std::to_string(r.version) + ", \"display_version\": \"" +
             jesc(r.display_version) + "\", \"build_id\": \"" + r.build_id + "\", \"files\": " + std::to_string(r.files) +
             ", \"bytes\": " + std::to_string(r.bytes) + ", \"zbic_modules\": " + std::to_string(r.zbic_modules) + "}");
        log_close();
        return 0;
    } catch (const Error& e) {
        line("@error " + one_line(e.what()));
        log_close();
        return exit_code(e.kind);
    } catch (const std::exception& e) {
        line("@error " + one_line(e.what()));
        log_close();
        return 1;
    }
}
