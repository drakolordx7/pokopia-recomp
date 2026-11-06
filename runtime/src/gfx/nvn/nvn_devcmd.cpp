// SPDX-License-Identifier: GPL-3.0-or-later
// Device command stream decoder and test-stream builder (nvn_devcmd.h, docs/nvn2_device_commands.md).
#include "nvn_devcmd.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace nxrt::nvn::devcmd {
namespace {

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// The sdk's stage values in the uniform-buffer macros (the table at sdk 0xBD9B4C, indexed by NVNshaderStage: vertex 8, tess control 4, tess eval 3,
// geometry 1, fragment 10, compute -1 (its own packet), task 9, mesh 11) and back.
constexpr uint32_t kStageValue[] = {8, 4, 3, 1, 10, 0xFFFFFFFFu, 9, 11};
bool stage_of_value(uint32_t v, uint32_t* stage) {
    for (uint32_t i = 0; i < sizeof kStageValue / sizeof kStageValue[0]; ++i)
        if (kStageValue[i] == v && i != 5) { *stage = i; return true; }
    return false;
}

void note_unhandled(Stats& st, uint32_t header) {
    ++st.unhandled;
    for (uint32_t& s : st.first_unhandled) {
        if (s == header) return;
        if (!s) { s = header; return; }
    }
}

// The decoding state: the registers the macros read, and the output.
struct Decoder {
    std::vector<Cmd>& out;
    Stats& st;
    int32_t base_vertex = 0;
    uint32_t index_type = 0;
    uint32_t index_hi = 0, index_lo = 0;
    uint32_t restart_enable = 0, restart_index = 0;
    uint64_t vb_addr[16] = {};
    uint32_t word = 0;   // word offset of the packet being decoded

    uint64_t index_address() const { return (static_cast<uint64_t>(index_hi) << 32) | index_lo; }

    void push(const Cmd& c) {
        Cmd d = c;
        d.word = word;
        out.push_back(d);
        ++st.commands;
    }

    void stencil(uint32_t what, uint32_t face, uint32_t value) {
        Cmd c;
        c.kind = Kind::SetStencil;
        c.what = what;
        c.face = face;
        c.value = value & 0xFF;
        push(c);
    }
    void restart() {
        Cmd c;
        c.kind = Kind::SetPrimitiveRestart;
        c.enable = restart_enable;
        c.value = restart_index;
        push(c);
    }

    // A write of `value` to register `method` by an IMMD or a data word of an INC / NON_INC packet. False when the register is not one we read.
    bool write(uint32_t method, uint32_t value) {
        switch (method) {
            case kRegBaseVertexA: case kRegBaseVertexB: base_vertex = static_cast<int32_t>(value); return true;
            case kRegIndexAddressHi: index_hi = value; return true;
            case kRegIndexAddressLo: index_lo = value; return true;
            case kRegIndexType: index_type = value; return true;
            case kRegRestartEnable: restart_enable = value != 0; restart(); return true;
            case kRegRestartIndex: restart_index = value; restart(); return true;
            case kRegStencilRefFront: stencil(0, 1, value); return true;
            case kRegStencilVMaskFront: stencil(1, 1, value); return true;
            case kRegStencilWMaskFront: stencil(2, 1, value); return true;
            case kRegStencilRefBack: stencil(0, 2, value); return true;
            case kRegStencilVMaskBack: stencil(1, 2, value); return true;
            case kRegStencilWMaskBack: stencil(2, 2, value); return true;
            default: return false;
        }
    }

    // An INC packet of `n` words: the two-word state packets (scissor, vertex buffer address and size), else single registers.
    bool inc(uint32_t method, const uint32_t* a, uint32_t n) {
        if (n == 2 && method >= kRegScissorBase && method < kRegScissorBase + 64 && ((method - kRegScissorBase) & 3) == 0) {
            Cmd c;
            c.kind = Kind::SetScissor;
            c.slot = (method - kRegScissorBase) / 4;
            const uint32_t x0 = a[0] & 0xFFFF, x1 = a[0] >> 16, y0 = a[1] & 0xFFFF, y1 = a[1] >> 16;
            c.rect[0] = static_cast<int32_t>(x0);
            c.rect[1] = static_cast<int32_t>(y0);
            c.rect[2] = x1 > x0 ? static_cast<int32_t>(x1 - x0) : 0;
            c.rect[3] = y1 > y0 ? static_cast<int32_t>(y1 - y0) : 0;
            push(c);
            return true;
        }
        if (n == 2 && method >= kRegVertexAddrBase && method < kRegVertexAddrBase + 64 && ((method - kRegVertexAddrBase) & 3) == 0) {
            vb_addr[(method - kRegVertexAddrBase) / 4] = (static_cast<uint64_t>(a[0]) << 32) | a[1];
            return true;
        }
        if (n == 2 && method >= kRegVertexSizeBase && method < kRegVertexSizeBase + 32 && ((method - kRegVertexSizeBase) & 1) == 0) {
            Cmd c;
            c.kind = Kind::BindVertexBuffer;
            c.slot = (method - kRegVertexSizeBase) / 2;
            c.address = vb_addr[c.slot];
            c.size = (static_cast<uint64_t>(a[0]) << 32) | a[1];
            push(c);
            return true;
        }
        bool all = n > 0;
        for (uint32_t k = 0; k < n; ++k) all = write(method + k, a[k]) && all;
        return all;
    }

    // One macro call: a = the arguments (the call word included), n = their count. False when the macro is not one we read.
    bool macro(uint32_t method, const uint32_t* a, uint32_t n) {
        Cmd c;
        uint32_t want = 0;
        switch (method) {
            case kMacroDrawArrays:
                want = 3;
                if (n == want) { c.kind = Kind::DrawArrays; c.mode = a[0]; c.first = a[1]; c.count = a[2]; }
                break;
            case kMacroDrawArraysInstanced: case kMacroDrawArraysInstancedBase: case kMacroDrawArraysIndirect:
                want = 5;
                if (n == want) {
                    c.kind = Kind::DrawArrays;
                    c.indirect = method == kMacroDrawArraysIndirect;
                    c.mode = a[0]; c.count = a[1]; c.instance_count = a[2]; c.first = a[3]; c.base_instance = a[4];
                }
                break;
            case kMacroDrawElements:
                want = 5;
                if (n == want) {
                    c.kind = Kind::DrawElements;
                    c.mode = a[0];
                    c.index_address = (static_cast<uint64_t>(a[1]) << 32) | a[2];
                    c.index_type = a[3];
                    c.count = a[4];
                    c.base_vertex = base_vertex;
                }
                break;
            case kMacroDrawElementsInstanced: case kMacroDrawElementsInstancedBase: case kMacroDrawElementsIndirect:
                want = 6;
                if (n == want) {
                    c.kind = Kind::DrawElements;
                    c.indirect = method == kMacroDrawElementsIndirect;
                    c.mode = a[0]; c.count = a[1]; c.instance_count = a[2]; c.first = a[3];
                    c.base_vertex = static_cast<int32_t>(a[4]);
                    c.base_instance = a[5];
                    c.index_type = index_type;
                    c.index_address = index_address();
                }
                break;
            case kMacroBindUniformBuffer:
                want = 4;
                if (n == want) {
                    c.kind = Kind::BindUniformBuffer;
                    if (!stage_of_value(a[0], &c.stage)) { ++st.bad_args; return true; }
                    c.slot = a[1];
                    c.address = (static_cast<uint64_t>(a[3] & 0xFF) << 32) | a[2];
                    c.size = static_cast<uint64_t>(a[3] >> 20) << 8;
                }
                break;
            case kMacroUnbindUniformBuffer:
                want = 2;
                if (n == want) {
                    c.kind = Kind::BindUniformBuffer;
                    if (!stage_of_value(a[0], &c.stage)) { ++st.bad_args; return true; }
                    c.slot = a[1];
                }
                break;
            case kMacroBindVertexBuffers: {
                if (n < 5 || (n - 1) % 4 != 0) { ++st.bad_args; return true; }
                const uint32_t count = (n - 1) / 4, first = a[0] & 0xFFFF;
                if ((a[0] >> 16) + 1 != count) { ++st.bad_args; return true; }
                for (uint32_t i = 0; i < count; ++i) {
                    Cmd v;
                    v.kind = Kind::BindVertexBuffer;
                    v.slot = first + i;
                    v.address = (static_cast<uint64_t>(a[2 + 4 * i]) << 32) | a[1 + 4 * i];
                    v.size = (static_cast<uint64_t>(a[4 + 4 * i]) << 32) | a[3 + 4 * i];
                    push(v);
                }
                return true;
            }
            default:
                return false;
        }
        if (n != want) { ++st.bad_args; return true; }
        push(c);
        return true;
    }
};

}  // namespace

const char* end_name(End e) {
    switch (e) {
        case End::Size: return "end of data";
        case End::EndSegment: return "END_PB_SEGMENT";
        case End::Zero: return "zero word (unwritten)";
        case End::BadFraming: return "unknown packet framing";
        case End::Truncated: return "packet runs past the end of the data";
    }
    return "?";
}

Stats decode(const uint8_t* bytes, size_t size, std::vector<Cmd>& out) {
    Stats st;
    Decoder d{out, st};
    const size_t n = size / 4;
    size_t i = 0;
    while (i < n) {
        const uint32_t h = rd32(bytes + i * 4);
        if (h == 0) { st.end = End::Zero; break; }
        const Header hd = decode_header(h);
        d.word = static_cast<uint32_t>(i);
        if (hd.sec_op == kSecEnd) { st.end = End::EndSegment; ++st.packets; ++i; break; }
        if (hd.sec_op == kSecImmd) {
            ++st.packets;
            if (!(hd.subch == 0 && d.write(hd.method, hd.count))) note_unhandled(st, h);
            ++i;
            continue;
        }
        if (hd.sec_op != kSecInc && hd.sec_op != kSecNonInc && hd.sec_op != kSecOneInc) { st.end = End::BadFraming; break; }
        if (i + 1 + hd.count > n) { st.end = End::Truncated; break; }
        ++st.packets;
        const uint32_t cnt = hd.count;
        std::vector<uint32_t> args(cnt);
        for (uint32_t k = 0; k < cnt; ++k) args[k] = rd32(bytes + (i + 1 + k) * 4);
        bool handled = false;
        if (hd.subch == 0 && cnt > 0) {
            if (hd.sec_op == kSecOneInc) handled = d.macro(hd.method, args.data(), cnt);
            else if (hd.sec_op == kSecInc) handled = d.inc(hd.method, args.data(), cnt);
            else handled = d.write(hd.method, args[cnt - 1]);   // NON_INC: every word goes to the same register, the last one stays
        }
        if (!handled) note_unhandled(st, h);
        i += 1 + cnt;
    }
    st.words = static_cast<uint32_t>(i);
    return st;
}

// ---- builder -----------------------------------------------------------------------------------------------------------------------
void StreamBuilder::one_inc(uint32_t method, std::initializer_list<uint32_t> args) {
    w_.push_back(make_header(kSecOneInc, static_cast<uint32_t>(args.size()), 0, method));
    for (uint32_t a : args) w_.push_back(a);
}

StreamBuilder& StreamBuilder::draw_arrays(uint32_t mode, uint32_t first, uint32_t count) {
    one_inc(kMacroDrawArrays, {mode, first, count});
    return *this;
}

StreamBuilder& StreamBuilder::draw_arrays_instanced(uint32_t mode, uint32_t first, uint32_t count, uint32_t base_instance, uint32_t instance_count) {
    if (base_instance) w_.push_back(make_header(kSecImmd, 0x190, 0, 0xE36));
    one_inc(base_instance ? kMacroDrawArraysInstancedBase : kMacroDrawArraysInstanced, {mode, count, instance_count, first, base_instance});
    return *this;
}

StreamBuilder& StreamBuilder::draw_elements(uint32_t mode, uint32_t type, uint32_t count, uint64_t index_address) {
    one_inc(kMacroDrawElements, {mode, static_cast<uint32_t>(index_address >> 32), static_cast<uint32_t>(index_address), type, count});
    return *this;
}

StreamBuilder& StreamBuilder::draw_elements_base_vertex(uint32_t mode, uint32_t type, uint32_t count, uint64_t index_address, int32_t base_vertex) {
    const uint32_t bv = static_cast<uint32_t>(base_vertex);
    w_.push_back(make_header(kSecInc, 1, 0, kRegBaseVertexA)); w_.push_back(bv);
    w_.push_back(make_header(kSecInc, 1, 0, kRegBaseVertexB)); w_.push_back(bv);
    draw_elements(mode, type, count, index_address);
    w_.push_back(make_header(kSecImmd, 0, 0, kRegBaseVertexA));
    w_.push_back(make_header(kSecImmd, 0, 0, kRegBaseVertexB));
    return *this;
}

StreamBuilder& StreamBuilder::draw_elements_instanced(uint32_t mode, uint32_t type, uint32_t count, uint64_t index_address, int32_t base_vertex, uint32_t base_instance,
                                                      uint32_t instance_count) {
    w_.push_back(make_header(kSecImmd, type, 0, kRegIndexType));
    w_.push_back(make_header(kSecInc, 2, 0, kRegIndexAddressHi));
    w_.push_back(static_cast<uint32_t>(index_address >> 32));
    w_.push_back(static_cast<uint32_t>(index_address));
    const bool based = base_vertex || base_instance;
    if (based) w_.push_back(make_header(kSecImmd, 0x190, 0, 0xE36));
    one_inc(based ? kMacroDrawElementsInstancedBase : kMacroDrawElementsInstanced, {mode, count, instance_count, 0u, static_cast<uint32_t>(base_vertex), base_instance});
    return *this;
}

StreamBuilder& StreamBuilder::draw_arrays_indirect(uint32_t mode, uint32_t count, uint32_t instance_count, uint32_t first, uint32_t base_instance) {
    w_.push_back(make_header(kSecImmd, 0x1110, 0, 2));   // the sdk's marker word (debug annotation) and the IMMD 0xE36 that precede the macro
    w_.push_back(make_header(kSecImmd, 0x190, 0, 0xE36));
    one_inc(kMacroDrawArraysIndirect, {mode, count, instance_count, first, base_instance});
    return *this;
}

StreamBuilder& StreamBuilder::draw_elements_indirect(uint32_t mode, uint32_t type, uint64_t index_address, uint32_t count, uint32_t instance_count, uint32_t first_index,
                                                     int32_t base_vertex, uint32_t base_instance) {
    w_.push_back(make_header(kSecImmd, 0x1111, 0, 2));
    w_.push_back(make_header(kSecImmd, 0x190, 0, 0xE36));
    w_.push_back(make_header(kSecImmd, type, 0, kRegIndexType));
    w_.push_back(make_header(kSecInc, 2, 0, kRegIndexAddressHi));
    w_.push_back(static_cast<uint32_t>(index_address >> 32));
    w_.push_back(static_cast<uint32_t>(index_address));
    one_inc(kMacroDrawElementsIndirect, {mode, count, instance_count, first_index, static_cast<uint32_t>(base_vertex), base_instance});
    return *this;
}

StreamBuilder& StreamBuilder::bind_vertex_buffer(uint32_t index, uint64_t address, uint64_t size) {
    w_.push_back(make_header(kSecInc, 2, 0, kRegVertexAddrBase + 4 * index));
    w_.push_back(static_cast<uint32_t>(address >> 32)); w_.push_back(static_cast<uint32_t>(address));
    w_.push_back(make_header(kSecInc, 2, 0, kRegVertexSizeBase + 2 * index));
    w_.push_back(static_cast<uint32_t>(size >> 32)); w_.push_back(static_cast<uint32_t>(size));
    return *this;
}

StreamBuilder& StreamBuilder::bind_vertex_buffers(uint32_t first, uint32_t count, const uint64_t* p) {
    if (!count) return *this;
    w_.push_back(make_header(kSecOneInc, 4 * count + 1, 0, kMacroBindVertexBuffers));
    w_.push_back(((count - 1) << 16) | first);
    for (uint32_t i = 0; i < count; ++i) {
        w_.push_back(static_cast<uint32_t>(p[2 * i])); w_.push_back(static_cast<uint32_t>(p[2 * i] >> 32));
        w_.push_back(static_cast<uint32_t>(p[2 * i + 1])); w_.push_back(static_cast<uint32_t>(p[2 * i + 1] >> 32));
    }
    return *this;
}

StreamBuilder& StreamBuilder::bind_uniform_buffer(uint32_t stage, uint32_t index, uint64_t address, uint64_t size) {
    const uint32_t sv = kStageValue[stage];
    if (!size) {
        one_inc(kMacroUnbindUniformBuffer, {sv, index});
    } else {
        const uint32_t packed = static_cast<uint32_t>(((size << 12) + 0xFF000) & 0xFFF00000u) | static_cast<uint32_t>((address >> 32) & 0xFF);
        one_inc(kMacroBindUniformBuffer, {sv, index, static_cast<uint32_t>(address), packed});
    }
    return *this;
}

StreamBuilder& StreamBuilder::set_stencil(uint32_t what, uint32_t face, uint32_t value) {
    static const uint32_t front[3] = {kRegStencilRefFront, kRegStencilVMaskFront, kRegStencilWMaskFront};
    static const uint32_t back[3] = {kRegStencilRefBack, kRegStencilVMaskBack, kRegStencilWMaskBack};
    if (face & 1) w_.push_back(make_header(kSecImmd, value & 0xFF, 0, front[what]));
    if (face & 2) w_.push_back(make_header(kSecImmd, value & 0xFF, 0, back[what]));
    return *this;
}

StreamBuilder& StreamBuilder::set_primitive_restart(bool enable, uint32_t index) {
    w_.push_back(make_header(kSecImmd, enable ? 1 : 0, 0, kRegRestartEnable));
    if (enable) { w_.push_back(make_header(kSecInc, 1, 0, kRegRestartIndex)); w_.push_back(index); }
    return *this;
}

StreamBuilder& StreamBuilder::set_scissor(uint32_t index, int32_t x, int32_t y, int32_t w, int32_t h) {
    uint32_t xw = 0, yw = 0;
    if (w > 0 && h > 0) {   // an empty rectangle is two zero words
        const int64_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(x) + w, 0xFFFF), y1 = std::min<int64_t>(static_cast<int64_t>(y) + h, 0xFFFF);
        if (x1 > x0 && y1 > y0) {
            xw = static_cast<uint32_t>(x0 | (x1 << 16));
            yw = static_cast<uint32_t>(y0 | (y1 << 16));
        }
    }
    w_.push_back(make_header(kSecInc, 2, 0, kRegScissorBase + 4 * index));
    w_.push_back(xw); w_.push_back(yw);
    return *this;
}

std::vector<uint8_t> StreamBuilder::bytes() const {
    std::vector<uint8_t> b(w_.size() * 4);
    if (!w_.empty()) std::memcpy(b.data(), w_.data(), b.size());
    return b;
}

}  // namespace nxrt::nvn::devcmd
