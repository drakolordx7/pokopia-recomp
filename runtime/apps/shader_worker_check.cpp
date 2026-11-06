// SPDX-License-Identifier: GPL-3.0-or-later
// Check of the on-demand shader translator client (src/gfx/nvn/nvn_shader_worker.cpp) against a real spirv_harness --serve, no game:
//   shader_worker_check <spirv_harness.exe> <corpus dir> <out cache dir> [blocks=8] [--no-corpus]
// Takes the first <blocks> rows of <corpus>/unique.tsv, sends each block's code and control bytes as the runtime does (with the corpus for
// its GLSLC reflection, or the bytes alone with --no-corpus), waits for the answers and checks that every answered block with status ok
// has <hash>.spv and a results.tsv row in <out cache dir>. Exit 0 = PASS.
// Build: clang-cl /std:c++20 /EHsc /O2 /I src/gfx/nvn apps/shader_worker_check.cpp src/gfx/nvn/nvn_shader_worker.cpp
#include "nvn_shader_worker.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

using nxrt::nvn::be::ShaderTranslatorClient;
namespace fs = std::filesystem;

static std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: shader_worker_check <spirv_harness.exe> <corpus dir> <out cache dir> [blocks] [--no-corpus]\n");
        return 2;
    }
    const fs::path corpus = argv[2];
    const int want = argc > 4 && argv[4][0] != '-' ? std::atoi(argv[4]) : 8;
    bool use_corpus = true;
    for (int i = 4; i < argc; ++i)
        if (std::string(argv[i]) == "--no-corpus") use_corpus = false;

    struct Block { std::string hash; std::vector<uint8_t> code, ctrl; };
    std::vector<Block> blocks;
    std::ifstream list(corpus / "unique.tsv");
    std::string line;
    std::getline(list, line);   // header: code_sha1_16 stage code_size ctrl_size occurrences ctrl_variants first_source entry_off glslc_off first_ctrl first_glslc
    while (static_cast<int>(blocks.size()) < want && std::getline(list, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string c; std::getline(ss, c, '\t');) f.push_back(c);
        if (f.size() < 11) continue;
        Block b{f[0], read_all(corpus / "code" / (f[0] + ".bin")), read_all(corpus / "ctrl" / (f[9] + ".bin"))};
        if (b.code.empty() || b.ctrl.empty()) continue;
        blocks.push_back(std::move(b));
    }
    if (blocks.empty()) { std::printf("FAIL: no blocks read from %s\n", (corpus / "unique.tsv").string().c_str()); return 1; }

    fs::create_directories(argv[3]);
    ShaderTranslatorClient::Options opt;
    opt.harness = argv[1];
    opt.cache_dir = argv[3];
    opt.corpus_dir = use_corpus ? corpus.string() : std::string();
    opt.jobs = 4;
    int rows = 0;
    ShaderTranslatorClient c;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!c.start(opt, [&](const std::string& h, const std::string& row, bool ok) { ++rows; std::printf("  %s %s  %.60s\n", h.c_str(), ok ? "ok  " : "FAIL", row.c_str()); },
                 [](bool warn, const std::string& t) { std::printf("  [%s] %s\n", warn ? "warn" : "info", t.c_str()); }, &err)) {
        std::printf("FAIL: %s\n", err.c_str());
        return 1;
    }
    for (const Block& b : blocks) c.submit(b.hash, b.code.data(), b.code.size(), b.ctrl.data(), b.ctrl.size());
    const bool dup = c.submit(blocks[0].hash, blocks[0].code.data(), blocks[0].code.size(), blocks[0].ctrl.data(), blocks[0].ctrl.size());
    while (c.pending() && c.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(180))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    int ok = 0, bad = 0;
    std::string results;
    { std::ifstream f(fs::path(argv[3]) / "results.tsv", std::ios::binary); std::stringstream s; s << f.rdbuf(); results = s.str(); }
    for (const Block& b : blocks) {
        const auto st = c.state(b.hash);
        const bool spv = fs::exists(fs::path(argv[3]) / (b.hash + ".spv"));
        const bool row = results.find("\n" + b.hash + "\t") != std::string::npos;
        if (st == ShaderTranslatorClient::State::Done && spv && row) ++ok;
        else if (st == ShaderTranslatorClient::State::Failed && row) ++ok;   // answered (e.g. unsupported): a row, no .spv
        else { ++bad; std::printf("  %s: state %d, spv %d, row %d\n", b.hash.c_str(), static_cast<int>(st), spv, row); }
    }
    c.stop();
    const bool pass = bad == 0 && !dup && rows == static_cast<int>(blocks.size()) && c.done() > 0;
    std::printf("%s: %zu blocks, %d answered with files in place, %llu translated, %llu failed, duplicate refused %d, %.1f s (process start included)\n",
                pass ? "PASS" : "FAIL", blocks.size(), ok, static_cast<unsigned long long>(c.done()), static_cast<unsigned long long>(c.failed()), !dup, secs);
    return pass ? 0 : 1;
}
