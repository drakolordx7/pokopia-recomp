// SPDX-License-Identifier: GPL-3.0-or-later
#include "extract.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "container.h"
#include "crypto.h"
#include "fileio.h"
#include "keys.h"
#include "log.h"
#include "nca.h"
#include "nso.h"
#include "pfs.h"
#include "romfs.h"
#include "zbic.h"   // runtime/src/loader (the runtime's own ZBIC decoder, compiled into this tool too)

namespace mprs {
namespace {

namespace fs = std::filesystem;

std::string to_win(std::string p) {
    for (char& c : p) if (c == '/') c = '\\';
    return p;
}

u32 le32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
void put32(u8* p, u32 v) { std::memcpy(p, &v, 4); }

bool never() { return false; }

void report(const ExtractOptions& o, const char* phase, u64 done, u64 total, const std::string& msg) {
    if (o.progress) o.progress(phase, done, total, msg);
}

void check_cancel(const ExtractOptions& o) {
    if (o.cancelled && o.cancelled()) fail(Fail::Cancelled, "Stopped by the user.");
}

// The display version from control.nacp (offset 0x3060, 16 bytes) of the control NCA, or "".
std::string display_version(const Nca* control) {
    if (!control) return "";
    try {
        const int rs = control->romfs_section();
        if (rs < 0) return "";
        auto rom = Romfs::open(control->open_romfs_image(rs));
        for (const RomfsFile& f : rom->files()) {
            if (f.path != "control.nacp" || f.size < 0x3070) continue;
            char v[17] = {};
            rom->open_file(f)->read(0x3060, v, 16);
            std::string s(v);
            if (s.find_first_not_of("0123456789.") != std::string::npos) return "";
            return s;
        }
    } catch (const std::exception& e) {
        MPRS_LOGW("Could not read the version from the dump: %s", e.what());
    }
    return "";
}

struct Opened {
    KeySet keys;
    std::vector<std::unique_ptr<Container>> c;
    DumpSet t;
    std::unique_ptr<Pfs> base_exefs, exefs;   // exefs: of the update when there is one, else of the base
    std::unique_ptr<Romfs> rom;
    ExtractResult r;
};

std::unique_ptr<Opened> open_inputs(const ExtractOptions& o) {
    auto d = std::make_unique<Opened>();
    if (o.key_files.empty()) fail(Fail::Keys, "No keys file was given. Pick your prod.keys file (dumped from your own console).");
    for (const std::string& f : o.key_files) d->keys.load_file(f);
    MPRS_LOGI("Keys loaded: %s", d->keys.describe().c_str());
    if (o.inputs.empty()) fail(Fail::Input, "No game dump file was given.");
    std::vector<const Container*> cs;
    for (size_t i = 0; i < o.inputs.size(); ++i) {
        report(o, "open", i, o.inputs.size(), "Opening " + o.inputs[i]);
        d->c.push_back(open_container(o.inputs[i]));
        cs.push_back(d->c.back().get());
        MPRS_LOGI("Input: %s, %s, %zu archives", o.inputs[i].c_str(), d->c.back()->kind.c_str(), d->c.back()->ncas.size());
    }
    d->t = select_titles(cs, d->keys, o.title_id);
    const Nca* base = d->t.base.get();
    const Nca* patch = d->t.patch.get();
    for (u32 v : d->t.other_patches) d->r.notes.push_back("An older update (v" + std::to_string(v) + ") was also given; the newest one is used.");
    for (const std::string& m : d->t.skipped) d->r.notes.push_back(m);
    for (size_t i = 0; i < o.inputs.size(); ++i)
        if (i < d->t.input_used.size() && !d->t.input_used[i]) d->r.notes.push_back("Nothing in " + o.inputs[i] + " belongs to this game; the file was not used.");

    d->base_exefs = base->open_pfs0(0);
    d->exefs = patch ? patch->open_pfs0(0) : base->open_pfs0(0);
    const PfsEntry* m = d->exefs->find("main");
    if (!m) fail(Fail::Input, "The dump's game program is incomplete (\"main\" is missing from its ExeFS).");
    d->r.build_id = nso_read_info(*d->exefs->open_file(*m)).build_id();
    for (const PfsEntry& e : d->exefs->entries()) {
        u8 h[16];
        if (e.size < 0x100) continue;
        d->exefs->open_file(e)->read(0, h, sizeof h);
        if (std::memcmp(h, "NSO0", 4) == 0 && (le32(h + 12) & nxrt::kNsoFlagZbic)) d->r.zbic_modules++;
    }
    d->r.version = patch ? d->t.patch_version : 0;
    d->r.display_version = display_version(d->t.control.get());
    d->r.dir = patch ? "updates/" + std::to_string(d->t.patch_version) : "base";

    const int rs = base->romfs_section();
    if (rs < 0) fail(Fail::Input, "The dump has no game data (RomFS) section.");
    if (patch) {
        const int prs = patch->romfs_section();
        if (prs < 0) fail(Fail::Input, "The update has no game data (RomFS) section.");
        d->rom = Romfs::open(patch->open_romfs_image(prs, base));
    } else {
        d->rom = Romfs::open(base->open_romfs_image(rs));
    }
    d->r.files = d->rom->files().size();
    d->r.bytes = d->rom->total_file_bytes();
    MPRS_LOGI("Title %s: %s%s, program build %s, RomFS %llu files, %llu bytes", o.title_id.c_str(), patch ? "update v" : "base game",
              patch ? std::to_string(d->r.version).c_str() : "", d->r.build_id.c_str(), (unsigned long long)d->r.files,
              (unsigned long long)d->r.bytes);
    return d;
}

void copy_storage(const IStorage& src, const std::string& dest, const ExtractOptions& o, std::atomic<u64>& done, bool keep = true) {
    const u64 size = src.size();
    if (keep && file_size_is(dest, size)) {
        done += size;
        return;
    }
    AtomicFileWriter w(dest);
    std::vector<u8> buf(static_cast<size_t>(std::min<u64>(4u << 20, std::max<u64>(size, 1))));
    for (u64 off = 0; off < size;) {
        check_cancel(o);
        const size_t n = static_cast<size_t>(std::min<u64>(buf.size(), size - off));
        src.read(off, buf.data(), n);
        w.write(buf.data(), n);
        off += n;
        done += n;
    }
    w.finish();
}

void write_exefs(const Pfs& exefs, const std::string& dir, const ExtractOptions& o) {
    if (!create_dirs(dir)) fail(Fail::Disk, "Cannot create the folder %s.", dir.c_str());
    std::atomic<u64> done{0};
    for (const PfsEntry& e : exefs.entries()) copy_storage(*exefs.open_file(e), path_join(dir, e.name), o, done);
}

void write_romfs(const Romfs& rom, const std::string& dir, const ExtractOptions& o) {
    if (!create_dirs(dir)) fail(Fail::Disk, "Cannot create the folder %s.", dir.c_str());
    for (const std::string& sub : rom.dirs())
        if (!create_dirs(path_join(dir, to_win(sub)))) fail(Fail::Disk, "Cannot create the folder %s.", sub.c_str());
    const auto& files = rom.files();
    // biggest first, so the threads finish together
    std::vector<size_t> order(files.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return files[a].size > files[b].size; });
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned n = static_cast<unsigned>(o.threads > 0 ? o.threads : std::min(8u, hw));
    std::atomic<size_t> next{0};
    std::atomic<u64> done{0};
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::exception_ptr first;
    auto worker = [&] {
        for (;;) {
            const size_t k = next++;
            if (k >= order.size() || stop) return;
            const RomfsFile& f = files[order[k]];
            try {
                copy_storage(*rom.open_file(f), path_join(dir, to_win(f.path)), o, done);
            } catch (...) {
                std::lock_guard lk(mu);
                if (!first) first = std::current_exception();
                stop = true;
                return;
            }
        }
    };
    std::vector<std::thread> ts;
    for (unsigned i = 0; i < std::min<size_t>(n, std::max<size_t>(files.size(), 1)); ++i) ts.emplace_back(worker);
    const u64 total = rom.total_file_bytes();
    std::atomic<bool> finished{false};
    std::thread ticker([&] {
        while (!finished) {
            report(o, "romfs", done, total, "Writing the game data (RomFS)");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    for (auto& t : ts) t.join();
    finished = true;
    ticker.join();
    if (first) std::rethrow_exception(first);
    report(o, "romfs", total, total, "Game data written");
}

std::string json_escape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        r += c;
    }
    return r;
}

// LZ4 block decompression of exactly dst_size bytes (as runtime/src/loader/nso.cpp).
bool lz4_block_decode(const u8* src, size_t n, u8* dst, size_t dst_size) {
    size_t ip = 0, op = 0;
    while (ip < n) {
        const unsigned token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= n) return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > n - ip || lit > dst_size - op) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= n) break;
        if (n - ip < 2) return false;
        const size_t off = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
        ip += 2;
        if (off == 0 || off > op) return false;
        size_t len = token & 15;
        if (len == 15) {
            unsigned b;
            do {
                if (ip >= n) return false;
                b = src[ip++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (len > dst_size - op) return false;
        for (size_t i = 0; i < len; i++) dst[op + i] = dst[op - off + i];
        op += len;
    }
    return op == dst_size;
}

}  // namespace

ExtractResult inspect_dump(const ExtractOptions& o) { return open_inputs(o)->r; }

ExtractResult extract_dump(const ExtractOptions& o) {
    if (o.out_dir.empty()) fail(Fail::Input, "No output folder was given.");
    auto d = open_inputs(o);
    const Nca* base = d->t.base.get();
    const Nca* patch = d->t.patch.get();
    const std::string out = to_win(o.out_dir);
    if (!create_dirs(out)) fail(Fail::Disk, "Cannot create the folder %s.", out.c_str());
    remove_part_files(out);
    const u64 need = d->r.bytes + (64ull << 20);
    const u64 have = free_disk_bytes(out);
    if (have && have < need)
        fail(Fail::Disk, "Not enough free space in %s: the game data needs %.1f GB, %.1f GB are free.", out.c_str(), need / 1e9, have / 1e9);

    // remove an old marker first: it is written again only when everything is in place
    std::error_code ec;
    fs::remove(fs::path(widen(path_join(out, "extract.json"))), ec);

    if (o.verify) {
        struct Sec { const Nca* n; int i; const Nca* b; };
        std::vector<Sec> secs = {{base, 0, nullptr}};
        if (patch) {
            secs.push_back({patch, 0, nullptr});
            secs.push_back({patch, patch->romfs_section(), base});
        } else {
            secs.push_back({base, base->romfs_section(), nullptr});
        }
        u64 total = 0, before = 0;
        for (const Sec& s : secs) total += s.n->section(s.i)->size;
        for (const Sec& s : secs) {
            const u64 sz = s.n->section(s.i)->size;
            s.n->verify_section(
                s.i, [&](u64 dn, u64 tot) { report(o, "verify", before + dn * sz / std::max<u64>(tot, 1), total, "Checking the dump"); },
                o.cancelled ? o.cancelled : std::function<bool()>(never), s.b);
            before += sz;
        }
    }

    report(o, "exefs", 0, 1, "Writing the game program (ExeFS)");
    write_exefs(*d->base_exefs, path_join(out, "base\\exefs"), o);
    const std::string dir = path_join(out, to_win(d->r.dir));
    if (patch) write_exefs(*d->exefs, path_join(dir, "exefs"), o);
    report(o, "exefs", 1, 1, "Game program written");
    write_romfs(*d->rom, path_join(dir, "romfs"), o);

    std::string j = "{\n  \"format\": 1,\n  \"title_id\": \"" + json_escape(o.title_id) + "\",\n  \"dir\": \"" + d->r.dir +
                    "\",\n  \"version\": " + std::to_string(d->r.version) + ",\n  \"display_version\": \"" +
                    json_escape(d->r.display_version) + "\",\n  \"build_id\": \"" + d->r.build_id + "\",\n  \"files\": " +
                    std::to_string(d->r.files) + ",\n  \"bytes\": " + std::to_string(d->r.bytes) + ",\n  \"zbic_modules\": " +
                    std::to_string(d->r.zbic_modules) + ",\n  \"inputs\": [";
    for (size_t i = 0; i < o.inputs.size(); ++i) {
        std::error_code e2;
        const auto sz = fs::file_size(fs::path(widen(o.inputs[i])), e2);
        j += std::string(i ? "," : "") + "\n    {\"path\": \"" + json_escape(o.inputs[i]) + "\", \"size\": " + std::to_string(e2 ? 0 : sz) + "}";
    }
    j += "\n  ]\n}\n";
    if (!write_text_file_atomic(path_join(out, "extract.json"), j)) fail(Fail::Disk, "Cannot write %s\\extract.json.", out.c_str());
    return d->r;
}

bool nso_is_zbic(const std::string& path) {
    try {
        auto f = open_file_storage(path);
        if (f->size() < 0x100) return false;
        u8 h[16];
        f->read(0, h, sizeof h);
        return std::memcmp(h, "NSO0", 4) == 0 && (le32(h + 12) & nxrt::kNsoFlagZbic);
    } catch (const Error&) {
        return false;
    }
}

void nso_to_plain(const std::string& in, const std::string& out) {
    auto f = open_file_storage(in);
    const u64 fsize = f->size();
    if (fsize < 0x100 || fsize > (1ull << 31)) fail(Fail::Input, "%s is not an NSO executable.", in.c_str());
    std::vector<u8> d = read_all(*f, 0, static_cast<size_t>(fsize));
    if (std::memcmp(d.data(), "NSO0", 4) != 0) fail(Fail::Input, "%s is not an NSO executable.", in.c_str());
    const u32 flags = le32(&d[0x0C]);
    const bool zbic = flags & nxrt::kNsoFlagZbic;
    std::vector<u8> seg[3];
    u32 first_off = 0xFFFFFFFF;
    for (int i = 0; i < 3; ++i) {
        const u32 foff = le32(&d[0x10 + 0x10 * i]), dsize = le32(&d[0x18 + 0x10 * i]), csize = le32(&d[0x60 + 4 * i]);
        const bool comp = (flags >> i) & 1;
        const u64 stored = comp ? csize : dsize;
        if (foff < 0x100 || foff > fsize || stored > fsize - foff) fail(Fail::Input, "%s: segment %d lies outside the file.", in.c_str(), i);
        first_off = std::min(first_off, foff);
        seg[i].resize(dsize);
        const u8* src = d.data() + foff;
        bool ok = true;
        if (!comp) std::memcpy(seg[i].data(), src, dsize);
        else if (zbic) ok = nxrt::zbic_decompress(seg[i].data(), dsize, src, csize);
        else ok = lz4_block_decode(src, csize, seg[i].data(), dsize);
        if (!ok) fail(Fail::Input, "%s: segment %d does not decompress (%s).", in.c_str(), i, zbic ? "ZBIC" : "LZ4");
        if ((flags >> (3 + i)) & 1) {
            Sha256 h;
            h.update(seg[i].data(), seg[i].size());
            if (std::memcmp(h.finish().data(), &d[0xA0 + 0x20 * i], 32) != 0)
                fail(Fail::Input, "%s: segment %d does not match its hash (the file is damaged).", in.c_str(), i);
        }
    }
    // header and whatever sits between it and the first segment (the module name), then the three segments back to back
    std::vector<u8> o(d.begin(), d.begin() + first_off);
    put32(&o[0x0C], flags & ~(0x7u | nxrt::kNsoFlagZbic));
    for (int i = 0; i < 3; ++i) {
        put32(&o[0x10 + 0x10 * i], static_cast<u32>(o.size()));
        put32(&o[0x60 + 4 * i], static_cast<u32>(seg[i].size()));
        o.insert(o.end(), seg[i].begin(), seg[i].end());
    }
    AtomicFileWriter w(out);
    w.write(o.data(), o.size());
    w.finish();
}

int exefs_to_plain(const std::string& in_dir, const std::string& out_dir) {
    if (!create_dirs(to_win(out_dir))) fail(Fail::Disk, "Cannot create the folder %s.", out_dir.c_str());
    int converted = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(widen(in_dir)), ec)) {
        if (!e.is_regular_file()) continue;
        const std::string name = narrow(e.path().filename().wstring());
        const std::string src = narrow(e.path().wstring());
        const std::string dst = path_join(to_win(out_dir), name);
        if (nso_is_zbic(src)) {
            nso_to_plain(src, dst);
            converted++;
        } else {
            auto s = open_file_storage(src);
            std::atomic<u64> done{0};
            ExtractOptions none;
            copy_storage(*s, dst, none, done, false);
        }
    }
    if (ec) fail(Fail::Input, "Cannot read the folder %s.", in_dir.c_str());
    return converted;
}

}  // namespace mprs
