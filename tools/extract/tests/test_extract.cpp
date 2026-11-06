// SPDX-License-Identifier: GPL-3.0-or-later
// nx_extract on synthetic dumps (tools/setup/tests builders, fake keys): a cartridge base game + an eShop update with a ticket gives
// base/exefs and updates/<v>/{exefs,romfs} with the update applied; an NSP base game alone gives base/{exefs,romfs}; and a ZBIC NSO
// made from known segments converts to a plain NSO with the same segments.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.h"
#include "common.h"
#include "extract.h"
#include "synth.h"
#include "synth_nca.h"

using namespace mprs;
using mprs_test::Bytes16;
using mprs_test::SynthFiles;
using mprs_test::SynthNcaSpec;
using mprs_test::SynthSection;
namespace fs = std::filesystem;

namespace {

#include "zbic_frames.inc"

constexpr u64 kApp = 0x0400A3A01EE4C000ull;
const char* const kTitleId = "0400A3A01EE4C000";
const char* const kBaseBuild = "0123456789ABCDEF0123456789ABCDEF01234567";
const char* const kNewBuild = "FEDCBA9876543210FEDCBA987654321000000000";

std::string tmp_dir(const char* name) {
    const fs::path p = fs::temp_directory_path() / (std::string("nxx_test_") + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p.string();
}

void write_file(const std::string& path, const std::vector<uint8_t>& d) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 8));
    return v;
}

void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

std::string tid_hex(u64 t) {
    char b[20];
    std::snprintf(b, sizeof b, "%016llX", (unsigned long long)t);
    return b;
}

std::string cid(int seed) {
    uint8_t b[16];
    for (int i = 0; i < 16; ++i) b[i] = uint8_t(seed * 31 + i * 17 + 1);
    return hex_encode(b, 16);
}

Bytes16 seeded(int s) {
    Bytes16 b;
    for (int i = 0; i < 16; ++i) b[i] = uint8_t(s * 13 + i * 29 + 3);
    return b;
}

struct Rec { std::string id; uint8_t type; };

std::vector<uint8_t> cnmt_bytes(u64 tid, u32 version, uint8_t type, u64 app_id, const std::vector<Rec>& recs) {
    const size_t ext = 0x10;
    std::vector<uint8_t> c(0x20 + ext + 0x38 * recs.size(), 0);
    put64(c.data(), tid);
    put32(c.data() + 8, version);
    c[0xC] = type;
    c[0xE] = uint8_t(ext);
    c[0x10] = uint8_t(recs.size());
    put64(c.data() + 0x20, app_id);
    for (size_t i = 0; i < recs.size(); ++i) {
        uint8_t* r = c.data() + 0x20 + ext + 0x38 * i;
        hex_decode(recs[i].id, r + 0x20, 16);
        r[0x30] = 0x10;
        r[0x36] = recs[i].type;
    }
    return c;
}

std::vector<uint8_t> meta_nca(u64 tid, const std::vector<uint8_t>& cnmt) {
    SynthNcaSpec sp;
    sp.content_type = 1;
    sp.program_id = tid;
    sp.sections = {SynthSection{1, 3, mprs_test::synth_pfs0({{"Meta_" + tid_hex(tid) + ".cnmt", cnmt}})}};
    return mprs_test::synth_nca(sp);
}

const SynthFiles kBaseFiles = {{"data/a.bin", pattern(40000, 1)}, {"data/b.bin", pattern(9000, 2)}, {"readme.txt", {'v', '1'}}};
const SynthFiles kNewFiles = {{"data/a.bin", pattern(40000, 1)},
                              {"data/b.bin", pattern(9000, 99)},
                              {"data/c.bin", pattern(90000, 5)},
                              {"readme.txt", {'v', '2'}}};

void check_tree(const std::string& dir, const SynthFiles& files) {
    for (const auto& [path, data] : files) {
        const auto got = read_file(dir + "/" + path);
        CHECK(got == data);
    }
}

void test_xci_plus_update(const std::string& root) {
    fs::create_directories(root);
    const std::string keys = mprs_test::write_synth_keys_file(root);
    SynthNcaSpec base;
    base.program_id = kApp;
    base.sections = {SynthSection{1, 3, mprs_test::synth_exefs(kBaseBuild)}, SynthSection{0, 3, mprs_test::synth_romfs(kBaseFiles)}};
    const mprs_test::SynthNcaBuild bb = mprs_test::synth_nca_build(base);

    SynthNcaSpec up;
    up.program_id = kApp;
    SynthSection rs{0, 4, mprs_test::synth_romfs(kNewFiles)};
    rs.patch_base = bb.plain[1];
    up.sections = {SynthSection{1, 3, mprs_test::synth_exefs(kNewBuild)}, rs};
    up.has_rights = true;
    up.rights_id = seeded(40);
    up.titlekey = seeded(41);
    const mprs_test::SynthNcaBuild ub = mprs_test::synth_nca_build(up);

    const u64 patch_id = kApp + 0x800;
    const std::string xci = root + "/game.xci", nsp = root + "/update.nsp";
    write_file(xci, mprs_test::synth_xci({{cid(1) + ".nca", bb.bytes},
                                          {cid(2) + ".cnmt.nca", meta_nca(kApp, cnmt_bytes(kApp, 0, 0x80, patch_id, {{cid(1), 1}}))}}));
    write_file(nsp, mprs_test::synth_nsp({{cid(7) + ".nca", ub.bytes},
                                          {cid(8) + ".cnmt.nca", meta_nca(patch_id, cnmt_bytes(patch_id, 0x20000, 0x81, kApp, {{cid(7), 1}}))},
                                          {hex_encode(seeded(40).data(), 16) + ".tik",
                                           mprs_test::synth_ticket(seeded(40), mprs_test::synth_encrypt_titlekey(11, seeded(41)), false)}}));

    ExtractOptions o;
    o.inputs = {nsp, xci};   // any order
    o.key_files = {keys};
    o.title_id = kTitleId;
    o.out_dir = root + "/out";
    const ExtractResult chk = inspect_dump(o);
    CHECK_EQ(chk.dir, std::string("updates/131072"));
    CHECK_EQ(chk.build_id, std::string(kNewBuild));
    CHECK(!fs::exists(o.out_dir));

    const ExtractResult r = extract_dump(o);
    CHECK_EQ(r.dir, std::string("updates/131072"));
    CHECK_EQ(r.version, 0x20000u);
    CHECK_EQ(r.files, u64(4));
    const std::string out = o.out_dir;
    for (const char* f : {"main", "rtld", "sdk", "subsdk0", "main.npdm"}) {
        CHECK(fs::is_regular_file(out + "/base/exefs/" + f));
        CHECK(fs::is_regular_file(out + "/updates/131072/exefs/" + f));
    }
    CHECK(!fs::exists(out + "/base/romfs"));
    const auto main_new = read_file(out + "/updates/131072/exefs/main");
    CHECK(main_new.size() >= 0x60 && hex_encode(main_new.data() + 0x40, 20, true) == kNewBuild);
    check_tree(out + "/updates/131072/romfs", kNewFiles);
    CHECK(fs::is_regular_file(out + "/extract.json"));
    const std::vector<uint8_t> j = read_file(out + "/extract.json");
    const std::string js(j.begin(), j.end());
    CHECK(js.find("\"dir\": \"updates/131072\"") != std::string::npos);

    // a second run keeps the files (sizes match) and ends the same
    const ExtractResult r2 = extract_dump(o);
    CHECK_EQ(r2.dir, r.dir);
    check_tree(out + "/updates/131072/romfs", kNewFiles);

    // the update alone is refused with a plain message
    ExtractOptions only = o;
    only.inputs = {nsp};
    only.out_dir = root + "/out2";
    CHECK_THROWS(extract_dump(only));
}

void test_nsp_base(const std::string& root) {
    fs::create_directories(root);
    const SynthFiles files = {{"x/y.bin", pattern(7000, 3)}, {"z.txt", {'h', 'i'}}};
    const mprs_test::SynthDump d = mprs_test::make_synth_dump("nsp", kApp, kBaseBuild, files, true, root);
    ExtractOptions o;
    o.inputs = {d.path};
    o.key_files = {d.keys_file_path};
    o.title_id = kTitleId;
    o.out_dir = root + "/out";
    const ExtractResult r = extract_dump(o);
    CHECK_EQ(r.dir, std::string("base"));
    CHECK(fs::is_regular_file(o.out_dir + "/base/exefs/main"));
    check_tree(o.out_dir + "/base/romfs", files);

    ExtractOptions other = o;
    other.title_id = "0100000000010000";
    other.out_dir = root + "/out_other";
    CHECK_THROWS(extract_dump(other));   // a different game
}

uint8_t zbic_pattern(size_t i, int seed) {
    static const char* w = "MOD0 text ro data bss nn::os::Thread nn::fs::Mount ";
    return (i % 5 == 0) ? uint8_t(seed + (i * i * 31) % 23) : uint8_t(w[(i / 3 + seed) % 50]);
}

void test_zbic_nso(const std::string& root) {
    const size_t sz[3] = {0x3000, 0x1800, 0x900};
    const uint8_t* fr[3] = {kSeg0, kSeg1, kSeg2};
    const size_t fn[3] = {sizeof kSeg0, sizeof kSeg1, sizeof kSeg2};
    std::vector<uint8_t> plain[3];
    for (int s = 0; s < 3; ++s) {
        plain[s].resize(sz[s]);
        for (size_t i = 0; i < sz[s]; ++i) plain[s][i] = zbic_pattern(i, s * 11 + 1);
    }
    std::vector<uint8_t> nso(0x100, 0);
    std::memcpy(nso.data(), "NSO0", 4);
    put32(&nso[0x0C], 0x3F | 0x80);
    uint32_t mem = 0;
    for (int s = 0; s < 3; ++s) {
        put32(&nso[0x10 + 0x10 * s], uint32_t(nso.size()));
        put32(&nso[0x14 + 0x10 * s], mem);
        put32(&nso[0x18 + 0x10 * s], uint32_t(sz[s]));
        put32(&nso[0x60 + 4 * s], uint32_t(fn[s]));
        const auto h = mprs_test::synth_sha256(plain[s].data(), plain[s].size());
        std::memcpy(&nso[0xA0 + 0x20 * s], h.data(), 32);
        nso.insert(nso.end(), fr[s], fr[s] + fn[s]);
        mem += uint32_t((sz[s] + 0xFFF) & ~size_t(0xFFF));
    }
    std::memset(&nso[0x40], 0xAB, 0x20);
    const std::string in = root + "/exefs", out = root + "/plain";
    write_file(in + "/main", nso);
    write_file(in + "/main.npdm", {'M', 'E', 'T', 'A'});
    CHECK(nso_is_zbic(in + "/main"));
    CHECK_EQ(exefs_to_plain(in, out), 1);
    const auto p = read_file(out + "/main");
    CHECK(p.size() == 0x100 + sz[0] + sz[1] + sz[2]);
    CHECK(!nso_is_zbic(out + "/main"));
    uint32_t flags;
    std::memcpy(&flags, &p[0x0C], 4);
    CHECK_EQ(flags, 0x38u);
    for (int s = 0; s < 3 && p.size() >= 0x100; ++s) {
        uint32_t off, csz;
        std::memcpy(&off, &p[0x10 + 0x10 * s], 4);
        std::memcpy(&csz, &p[0x60 + 4 * s], 4);
        CHECK_EQ(csz, uint32_t(sz[s]));
        CHECK(off + sz[s] <= p.size() && std::memcmp(&p[off], plain[s].data(), sz[s]) == 0);
    }
    CHECK(read_file(out + "/main.npdm").size() == 4);

    nso[0x100 + 40] ^= 0x5A;   // damage the text frame: refused, not written as plain
    write_file(in + "/bad", nso);
    CHECK_THROWS(nso_to_plain(in + "/bad", out + "/bad"));
    CHECK(!fs::exists(out + "/bad"));
}

}  // namespace

int main() {
    const std::string root = tmp_dir("extract");
    try {
        test_xci_plus_update(root + "/a");
        test_nsp_base(root + "/b");
        test_zbic_nso(root + "/c");
    } catch (const std::exception& e) {
        std::printf("FAIL: exception %s\n", e.what());
        ++mprs_test::failures();
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return TEST_SUMMARY("test_extract");
}
