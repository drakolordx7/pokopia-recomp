// SPDX-License-Identifier: GPL-3.0-or-later
// NVN2 device commands (nvnCommandBufferBeginDeviceCommands / EndDeviceCommands / SubmitDeviceCommands): the stream a device command
// buffer holds, and the CPU decoder the replay uses for it. Format and evidence: docs/nvn2_device_commands.md.
//
// A device command buffer's storage is   [0, 256)                     header (cursors, written by the console's GPU; the replay ignores it)
//                                        [256, 256 + command size)    command words: the same push-buffer packets the sdk's command writers
//                                                                     put in command memory
//                                        [aligned end, ...)           "compute memory": inline compute launch records (not decoded yet)
// This file knows the packet framing and the draw packets; no Vulkan, no NVN objects, so tests build streams with StreamBuilder and
// check what the decoder returns.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxrt::nvn::devcmd {

constexpr uint32_t kHeaderBytes = 256;          // bytes of header before the command words (the sdk's Begin writes 256, Submit calls storage + 256)
constexpr uint32_t kSegmentMaxBytes = 8388604;  // 0x7FFFFC: the longest push-buffer segment (21-bit dword count); the sdk splits a stream here

// ---- push-buffer packet framing (header word: sec_op[31:29] count_or_data[28:16] subchannel[15:13] method[11:0], methods in dwords) --
enum SecOp : uint32_t { kSecGrp0 = 0, kSecInc = 1, kSecGrp2 = 2, kSecNonInc = 3, kSecImmd = 4, kSecOneInc = 5, kSecReserved = 6, kSecEnd = 7 };
struct Header {
    uint32_t sec_op, count, subch, method;   // count: the data word count, or the immediate value for kSecImmd
};
constexpr Header decode_header(uint32_t h) { return {h >> 29, (h >> 16) & 0x1FFF, (h >> 13) & 7, h & 0xFFF}; }
constexpr uint32_t make_header(uint32_t sec_op, uint32_t count, uint32_t subch, uint32_t method) {
    return (sec_op << 29) | ((count & 0x1FFF) << 16) | ((subch & 7) << 13) | (method & 0xFFF);
}

// ---- NVN2 macro methods (ONE_INC packets on subchannel 0: the first word calls the macro, the rest are its arguments) ---------------
// Verified against the sdk's command writers (docs/nvn2_device_commands.md has the addresses).
enum Method : uint32_t {
    kMacroDrawElements = 0xE04,              // mode, index address hi, lo, index type, count
    kMacroDrawArrays = 0xE4A,                // mode, first, count
    kMacroDrawArraysIndirect = 0xE4C,        // mode, count, instance count, first, base instance (NVNdrawArraysIndirectData, inline)
    kMacroDrawArraysInstanced = 0xE4E,       // same five arguments as the indirect form; base instance 0
    kMacroDrawElementsIndirect = 0xE50,      // mode, count, instance count, first index, base vertex, base instance (NVNdrawElementsIndirectData, inline)
    kMacroDrawElementsInstanced = 0xE52,     // same six arguments; base vertex and base instance are 0
    kMacroDrawArraysInstancedBase = 0xE9E,   // kMacroDrawArraysInstanced after an IMMD 0xE36 (the sdk uses it when the base instance is not 0)
    kMacroDrawElementsInstancedBase = 0xEA0, // kMacroDrawElementsInstanced, same
};
// Registers the draw packets read (state the macros use): written by IMMD or INC packets.
enum Reg : uint32_t {
    kRegBaseVertexA = 0x446,    // DrawElementsBaseVertex writes the base vertex to both, then 0 after the draw
    kRegBaseVertexB = 0x50D,
    kRegIndexAddressHi = 0x5F2, // INC, two words: index buffer address hi, lo (DrawElementsInstanced / Indirect)
    kRegIndexAddressLo = 0x5F3,
    kRegIndexType = 0x5F6,      // IMMD: NVNindexType
    kRegRestartEnable = 0x591,  // IMMD 0 / 1 (SetPrimitiveRestart)
    kRegRestartIndex = 0x592,   // INC, one word (written only when enabling)
    kRegStencilRefFront = 0x4E5, kRegStencilVMaskFront = 0x4E6, kRegStencilWMaskFront = 0x4E7,   // IMMD, 8-bit value (SetStencilRef / ValueMask / Mask)
    kRegStencilRefBack = 0x3D5, kRegStencilWMaskBack = 0x3D6, kRegStencilVMaskBack = 0x3D7,
    kRegScissorBase = 0x381,    // INC, two words per scissor at 0x381 + 4 * index: x min | x max << 16, y min | y max << 16
    kRegVertexAddrBase = 0x701, // INC, two words per vertex buffer at 0x701 + 4 * index: address hi, lo
    kRegVertexSizeBase = 0x180, // INC, two words per vertex buffer at 0x180 + 2 * index: size hi, lo
};
// More macros: buffer binds.
enum Macro2 : uint32_t {
    kMacroBindUniformBuffer = 0xE02,         // stage value, index, address lo, (size in 256s) << 20 | address hi (8 bits)
    kMacroUnbindUniformBuffer = 0xE00,       // stage value, index
    kMacroBindVertexBuffers = 0xE98,         // (count - 1) << 16 | first, then count * {address lo, hi, size lo, hi}
};

// ---- decoded commands ---------------------------------------------------------------------------------------------------------------
enum class Kind : uint32_t { DrawArrays, DrawElements, BindVertexBuffer, BindUniformBuffer, SetStencil, SetPrimitiveRestart, SetScissor };
struct Cmd {
    Kind kind = Kind::DrawArrays;
    bool indirect = false;          // the packet carried an NVN indirect-data struct inline (DrawArraysIndirect / DrawElementsIndirect)
    uint32_t mode = 0;              // NVNdrawPrimitive, as the sdk stores it
    uint32_t count = 0;             // vertices (arrays) or indices (elements)
    uint32_t instance_count = 1;
    uint32_t first = 0;             // arrays: first vertex; elements: first index
    uint32_t base_instance = 0;
    int32_t base_vertex = 0;
    uint32_t index_type = 0;        // NVNindexType (elements)
    uint64_t index_address = 0;     // GPU address of the index buffer (elements); `first` is not applied to it
    uint32_t word = 0;              // word offset of the packet in the command words
    // The non-draw commands (Kind says which fields are set):
    uint32_t slot = 0;              // BindVertexBuffer / BindUniformBuffer: binding index; SetScissor: scissor index
    uint32_t stage = 0;             // BindUniformBuffer: NVNshaderStage
    uint64_t address = 0, size = 0; // BindVertexBuffer / BindUniformBuffer (a uniform size is rounded up to 256 bytes by the sdk's encoding; 0 unbinds)
    uint32_t what = 0;              // SetStencil: 0 ref, 1 value mask, 2 write mask (P_Stencil::what)
    uint32_t face = 0;              // SetStencil: NVNface (1 front, 2 back)
    uint32_t value = 0;             // SetStencil: the value
    uint32_t enable = 0;            // SetPrimitiveRestart (the index is in `value`)
    int32_t rect[4] = {};           // SetScissor: x, y, width, height
};

enum class End : uint32_t { Size, EndSegment, Zero, BadFraming, Truncated };   // why decoding stopped
struct Stats {
    uint32_t words = 0;             // words consumed
    uint32_t packets = 0;           // packets read (recognised or not)
    uint32_t commands = 0;          // decoded commands (draws, binds, state)
    uint32_t unhandled = 0;         // packets that are not decoded: state writes, markers, other macros
    uint32_t bad_args = 0;          // macros whose argument count is not the one the sdk writes
    uint32_t first_unhandled[4] = {};   // headers of the first four distinct unhandled packets (logs)
    End end = End::Size;
};

// Decodes `size` bytes of command words (little endian). Appends to `out`. Stops at the end of the data, an END_PB_SEGMENT packet, a zero word
// (memory nobody wrote) or a packet with unknown framing (sec_op 0, 2 or 6).
Stats decode(const uint8_t* bytes, size_t size, std::vector<Cmd>& out);
const char* end_name(End e);

// ---- building streams (tests; a host-side producer of the same packets the sdk writes) --------------------------------------------------
class StreamBuilder {
public:
    // Every method appends the words the sdk's writer of that command appends (docs/nvn2_device_commands.md).
    StreamBuilder& draw_arrays(uint32_t mode, uint32_t first, uint32_t count);
    StreamBuilder& draw_arrays_instanced(uint32_t mode, uint32_t first, uint32_t count, uint32_t base_instance, uint32_t instance_count);
    StreamBuilder& draw_elements(uint32_t mode, uint32_t type, uint32_t count, uint64_t index_address);
    StreamBuilder& draw_elements_base_vertex(uint32_t mode, uint32_t type, uint32_t count, uint64_t index_address, int32_t base_vertex);
    StreamBuilder& draw_elements_instanced(uint32_t mode, uint32_t type, uint32_t count, uint64_t index_address, int32_t base_vertex, uint32_t base_instance,
                                           uint32_t instance_count);
    // The indirect forms with the NVN indirect-data struct written inline (what the console's control stream splices in from the buffer).
    StreamBuilder& draw_arrays_indirect(uint32_t mode, uint32_t count, uint32_t instance_count, uint32_t first, uint32_t base_instance);
    StreamBuilder& draw_elements_indirect(uint32_t mode, uint32_t type, uint64_t index_address, uint32_t count, uint32_t instance_count, uint32_t first_index,
                                          int32_t base_vertex, uint32_t base_instance);
    StreamBuilder& bind_vertex_buffer(uint32_t index, uint64_t address, uint64_t size);          // the INC pairs of nvnCommandBufferBindVertexBuffer
    StreamBuilder& bind_vertex_buffers(uint32_t first, uint32_t count, const uint64_t* address_size_pairs);   // macro 0xE98
    StreamBuilder& bind_uniform_buffer(uint32_t stage, uint32_t index, uint64_t address, uint64_t size);    // macro 0xE02 / 0xE00 (graphics stages)
    StreamBuilder& set_stencil(uint32_t what, uint32_t face, uint32_t value);
    StreamBuilder& set_primitive_restart(bool enable, uint32_t index);
    StreamBuilder& set_scissor(uint32_t index, int32_t x, int32_t y, int32_t w, int32_t h);
    StreamBuilder& raw(uint32_t word) { w_.push_back(word); return *this; }
    StreamBuilder& end_segment() { w_.push_back(make_header(kSecEnd, 0, 0, 0)); return *this; }
    const std::vector<uint32_t>& words() const { return w_; }
    std::vector<uint8_t> bytes() const;
private:
    void one_inc(uint32_t method, std::initializer_list<uint32_t> args);
    std::vector<uint32_t> w_;
};

}  // namespace nxrt::nvn::devcmd
