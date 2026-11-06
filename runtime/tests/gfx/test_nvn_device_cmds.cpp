// SPDX-License-Identifier: GPL-3.0-or-later
// NVN2 device commands (docs/nvn2_device_commands.md): the stream decoder against words the sdk's command writers produce, streams built in
// memory, and the entry points (device command buffer object, Begin / End / Submit) through the proc table.
#include <cstring>
#include <string>
#include <vector>

#include "nvn_cmd.h"
#include "nvn_devcmd.h"
#include "nvn_impl.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_device_cmds)

using namespace nxrt;
using namespace nxrt::nvn;
using namespace nxrt::nvn::be;
namespace dc = nxrt::nvn::devcmd;

namespace {

std::vector<dc::Cmd> decode_all(const dc::StreamBuilder& b, dc::Stats* st = nullptr) {
    std::vector<dc::Cmd> out;
    const std::vector<uint8_t> bytes = b.bytes();
    const dc::Stats s = dc::decode(bytes.data(), bytes.size(), out);
    if (st) *st = s;
    return out;
}

}  // namespace

NXTEST(framing_matches_the_words_the_sdk_writes) {
    // Headers read off the sdk's writers (BeginDeviceCommands 0xA0080E9C, DrawArrays 0xA0030E4A, ...).
    const dc::Header a = dc::decode_header(0xA0030E4Au);
    CHECK_EQ(a.sec_op, static_cast<uint32_t>(dc::kSecOneInc));
    CHECK_EQ(a.count, 3u);
    CHECK_EQ(a.subch, 0u);
    CHECK_EQ(a.method, 0xE4Au);
    const dc::Header b = dc::decode_header(0xA0082E9Cu);   // Begin on a compute buffer: subchannel 1
    CHECK_EQ(b.sec_op, 5u);
    CHECK_EQ(b.count, 8u);
    CHECK_EQ(b.subch, 1u);
    CHECK_EQ(b.method, 0xE9Cu);
    const dc::Header m = dc::decode_header(0x91180002u);  // the mode marker the sdk puts before End: IMMD, data 0x1118, method 2
    CHECK_EQ(m.sec_op, 4u);
    CHECK_EQ(m.count, 0x1118u);
    CHECK_EQ(m.method, 2u);
    CHECK_EQ(dc::make_header(dc::kSecOneInc, 3, 0, 0xE4A), 0xA0030E4Au);
    CHECK_EQ(dc::make_header(dc::kSecOneInc, 6, 0, 0xE50), 0xA0060E50u);
    CHECK_EQ(dc::make_header(dc::kSecImmd, 0x190, 0, 0xE36), 0x81900E36u);
    CHECK_EQ(dc::make_header(dc::kSecImmd, 0x1110, 0, 2), 0x91100002u);
}

NXTEST(draw_arrays_words_and_decode) {
    dc::StreamBuilder b;
    b.draw_arrays(4, 7, 9);
    // sdk nvnCommandBufferDrawArrays: header, mode, first, count
    REQUIRE(b.words().size() == 4u);
    CHECK_EQ(b.words()[0], 0xA0030E4Au);
    CHECK_EQ(b.words()[1], 4u);
    CHECK_EQ(b.words()[2], 7u);
    CHECK_EQ(b.words()[3], 9u);
    dc::Stats st;
    const auto c = decode_all(b, &st);
    REQUIRE(c.size() == 1u);
    CHECK(c[0].kind == dc::Kind::DrawArrays);
    CHECK_EQ(c[0].mode, 4u);
    CHECK_EQ(c[0].first, 7u);
    CHECK_EQ(c[0].count, 9u);
    CHECK_EQ(c[0].instance_count, 1u);
    CHECK_EQ(c[0].base_instance, 0u);
    CHECK(!c[0].indirect);
    CHECK_EQ(st.commands, 1u);
    CHECK_EQ(st.unhandled, 0u);
    CHECK(st.end == dc::End::Size);
}

NXTEST(mixed_stream_decodes_in_order) {
    dc::StreamBuilder b;
    b.draw_arrays(4, 0, 3)
        .draw_arrays_instanced(4, 5, 6, 0, 2)                   // 0xE4E
        .draw_arrays_instanced(4, 5, 6, 9, 2)                   // 0xE9E behind IMMD 0xE36
        .draw_elements(4, 2, 36, 0x7100001000ull)               // 0xE04
        .draw_elements_base_vertex(4, 2, 12, 0x7100002000ull, -5)
        .draw_elements_instanced(4, 1, 6, 0x7100003000ull, 0, 0, 3)
        .draw_elements_instanced(4, 1, 6, 0x7100003000ull, -2, 8, 3)
        .draw_arrays_indirect(4, 100, 2, 10, 3)
        .draw_elements_indirect(4, 2, 0x7100004000ull, 60, 4, 5, -1, 6)
        .draw_arrays(5, 1, 1);
    dc::Stats st;
    const auto c = decode_all(b, &st);
    CHECK_EQ(st.bad_args, 0u);
    CHECK(st.end == dc::End::Size);
    REQUIRE(c.size() == 10u);

    CHECK(c[0].kind == dc::Kind::DrawArrays && c[0].first == 0 && c[0].count == 3);
    // DrawArraysInstanced(mode, first 5, count 6, base_instance, instance_count 2)
    CHECK(c[1].kind == dc::Kind::DrawArrays);
    CHECK_EQ(c[1].first, 5u); CHECK_EQ(c[1].count, 6u); CHECK_EQ(c[1].instance_count, 2u); CHECK_EQ(c[1].base_instance, 0u);
    CHECK_EQ(c[2].base_instance, 9u); CHECK_EQ(c[2].instance_count, 2u); CHECK_EQ(c[2].first, 5u);
    // DrawElements: address, type and count inline
    CHECK(c[3].kind == dc::Kind::DrawElements);
    CHECK_EQ(c[3].index_address, 0x7100001000ull); CHECK_EQ(c[3].index_type, 2u); CHECK_EQ(c[3].count, 36u); CHECK_EQ(c[3].base_vertex, 0);
    // DrawElementsBaseVertex: the base vertex comes from the registers written before the macro, and is 0 again after the draw
    CHECK_EQ(c[4].base_vertex, -5); CHECK_EQ(c[4].index_address, 0x7100002000ull); CHECK_EQ(c[4].count, 12u);
    // DrawElementsInstanced: index address and type come from registers; base vertex, base instance and instance count from the macro
    CHECK_EQ(c[5].index_address, 0x7100003000ull); CHECK_EQ(c[5].index_type, 1u); CHECK_EQ(c[5].instance_count, 3u);
    CHECK_EQ(c[5].base_vertex, 0); CHECK_EQ(c[5].base_instance, 0u);
    CHECK_EQ(c[6].base_vertex, -2); CHECK_EQ(c[6].base_instance, 8u); CHECK_EQ(c[6].instance_count, 3u); CHECK_EQ(c[6].count, 6u);
    // indirect forms: NVNdrawArraysIndirectData {count, instanceCount, first, baseInstance} inline
    CHECK(c[7].indirect && c[7].kind == dc::Kind::DrawArrays);
    CHECK_EQ(c[7].count, 100u); CHECK_EQ(c[7].instance_count, 2u); CHECK_EQ(c[7].first, 10u); CHECK_EQ(c[7].base_instance, 3u);
    // NVNdrawElementsIndirectData {count, instanceCount, firstIndex, baseVertex, baseInstance}
    CHECK(c[8].indirect && c[8].kind == dc::Kind::DrawElements);
    CHECK_EQ(c[8].count, 60u); CHECK_EQ(c[8].instance_count, 4u); CHECK_EQ(c[8].first, 5u); CHECK_EQ(c[8].base_vertex, -1); CHECK_EQ(c[8].base_instance, 6u);
    CHECK_EQ(c[8].index_address, 0x7100004000ull); CHECK_EQ(c[8].index_type, 2u);
    // the packet after all of that is read from the right place: the registers did not leak into a direct draw
    CHECK(c[9].kind == dc::Kind::DrawArrays && c[9].mode == 5 && c[9].first == 1 && c[9].count == 1);
    // the IMMD markers and 0xE36 selectors are counted, not decoded
    CHECK(st.unhandled >= 3u);
    CHECK_EQ(st.commands, 10u);
}

NXTEST(decoding_stops_where_it_should) {
    {   // END_PB_SEGMENT: whatever follows is not read
        dc::StreamBuilder b;
        b.draw_arrays(4, 0, 3).end_segment().draw_arrays(4, 1, 3);
        dc::Stats st;
        const auto c = decode_all(b, &st);
        CHECK_EQ(c.size(), 1u);
        CHECK(st.end == dc::End::EndSegment);
    }
    {   // memory nobody wrote reads as zero words
        dc::StreamBuilder b;
        b.draw_arrays(4, 0, 3).raw(0).raw(0).draw_arrays(4, 1, 3);
        dc::Stats st;
        CHECK_EQ(decode_all(b, &st).size(), 1u);
        CHECK(st.end == dc::End::Zero);
    }
    {   // a packet that runs past the data
        dc::StreamBuilder b;
        b.raw(dc::make_header(dc::kSecOneInc, 5, 0, dc::kMacroDrawElements)).raw(4);
        dc::Stats st;
        CHECK_EQ(decode_all(b, &st).size(), 0u);
        CHECK(st.end == dc::End::Truncated);
    }
    {   // a draw macro with the wrong argument count is counted and skipped; the next packet is still read
        dc::StreamBuilder b;
        b.raw(dc::make_header(dc::kSecOneInc, 2, 0, dc::kMacroDrawArrays)).raw(4).raw(5).draw_arrays(4, 2, 3);
        dc::Stats st;
        const auto c = decode_all(b, &st);
        CHECK_EQ(st.bad_args, 1u);
        REQUIRE(c.size() == 1u);
        CHECK_EQ(c[0].first, 2u);
    }
    {   // sec_op 2 (GRP2_USE_TERT): the length is unknown
        dc::StreamBuilder b;
        b.draw_arrays(4, 0, 3).raw(0x40000E4Au);
        dc::Stats st;
        decode_all(b, &st);
        CHECK(st.end == dc::End::BadFraming);
    }
    {   // an unrelated state packet is skipped by its length
        dc::StreamBuilder b;
        b.raw(dc::make_header(dc::kSecInc, 2, 0, 0x300)).raw(1).raw(2).draw_arrays(4, 0, 3);
        dc::Stats st;
        CHECK_EQ(decode_all(b, &st).size(), 1u);
        CHECK_EQ(st.unhandled, 1u);
    }
}

namespace {

void setup() {
    install();
    abi::reset_mismatches();
    register_api_cmdbuf();
    abi::reset_mismatches();   // register_api_cmdbuf reports a mismatch of its own (nvnCommandBufferGetMemoryCallbackData)
    register_api_cmd_work();
    register_api_device_cmds();
}

template <class Sig>
abi::Caller<Sig> nvn2(const char* name) { return abi::Caller<Sig>(proc_address(name)); }

}  // namespace

NXTEST(entry_points_record_one_submit_with_the_storage_addresses) {
    setup();
    auto init = abi::proc<PFNNVNCOMMANDBUFFERINITIALIZEPROC>("nvnCommandBufferInitialize");
    auto begin_rec = abi::proc<PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC>("nvnCommandBufferBeginRecording");
    auto end_rec = abi::proc<PFNNVNCOMMANDBUFFERENDRECORDINGPROC>("nvnCommandBufferEndRecording");
    auto set_defaults = nvn2<void(void*)>("nvnDeviceCommandBufferSetDefaults");
    auto set_device = nvn2<void(void*, void*)>("nvnDeviceCommandBufferSetDevice");
    auto set_storage = nvn2<void(void*, void*, int64_t, int64_t)>("nvnDeviceCommandBufferSetStorage");
    auto set_cmd = nvn2<void(void*, int64_t)>("nvnDeviceCommandBufferSetCommandMemorySize");
    auto set_comp = nvn2<void(void*, int64_t)>("nvnDeviceCommandBufferSetComputeMemorySize");
    auto get_handle = nvn2<uint64_t(void*)>("nvnDeviceCommandBufferGetHandle");
    auto begin_dev = nvn2<void(NVNcommandBuffer*, void*)>("nvnCommandBufferBeginDeviceCommands");
    auto end_dev = nvn2<void(NVNcommandBuffer*, void*)>("nvnCommandBufferEndDeviceCommands");
    auto submit_dev = nvn2<void(NVNcommandBuffer*, void*)>("nvnCommandBufferSubmitDeviceCommands");
    REQUIRE(static_cast<bool>(set_defaults) && static_cast<bool>(get_handle) && static_cast<bool>(begin_dev) && static_cast<bool>(end_dev) && static_cast<bool>(submit_dev));
    CHECK_EQ(abi::mismatches(), 0u);

    uint64_t storage[6];   // the guest's 0x30 bytes
    static_assert(sizeof storage == 0x30, "");
    std::memset(storage, 0xAB, sizeof storage);
    NVNmemoryPool guest_pool{};
    auto* pool = new MemoryPool();
    pool->gpu_base = 0x7200000000ull;
    pool->size = 1 << 20;
    bind_guest(&guest_pool, pool);

    set_defaults(storage);
    set_device(storage, nullptr);
    CHECK_EQ(get_handle(storage), 0ull);                              // no pool yet
    set_storage(storage, &guest_pool, 0x4000, 0x10000);
    set_cmd(storage, 0x1000);
    set_comp(storage, 0x800);
    CHECK_EQ(storage[1], reinterpret_cast<uint64_t>(&guest_pool));   // layout as the sdk keeps it
    CHECK_EQ(storage[2], 0x4000ull);
    CHECK_EQ(storage[3], 0x10000ull);
    CHECK_EQ(storage[4], 0x1000ull);
    CHECK_EQ(storage[5], 0x800ull);
    CHECK_EQ(get_handle(storage), 0x7200004000ull);                  // pool address + offset

    NVNcommandBuffer c;
    std::memset(&c, 0, sizeof c);
    CHECK(init(&c, nullptr));
    begin_dev(&c, storage);   // outside a recording: nothing happens
    submit_dev(&c, storage);
    begin_rec(&c);
    begin_dev(&c, storage);
    end_dev(&c, storage);
    submit_dev(&c, storage);
    NVNcommandHandle h = end_rec(&c);

    Ref<CmdList> list = handle_lookup(h);
    REQUIRE(static_cast<bool>(list));
    CHECK_EQ(list->record_count, 1u);
    RecordReader r(*list);
    REQUIRE(r.next());
    CHECK(r.op() == Op::SubmitDeviceCommands);
    const auto& p = r.payload<P_SubmitDeviceCommands>();
    CHECK_EQ(p.stream_address, 0x7200004000ull + dc::kHeaderBytes);
    CHECK_EQ(p.stream_size, 0x1000ull);
    CHECK_EQ(p.compute_address, 0x7200004000ull + 0x1100);          // (0x1000 + 511) & ~255
    CHECK_EQ(p.compute_size, 0x800ull);
    CHECK(!r.next());
    CHECK_EQ(std::string(op_name(Op::SubmitDeviceCommands)), std::string("SubmitDeviceCommands"));
}

NXTEST(a_stream_decodes_to_what_the_direct_calls_record) {
    // The draws a device stream carries are the ones the direct API calls record: build both from the same arguments and compare.
    setup();
    auto init = abi::proc<PFNNVNCOMMANDBUFFERINITIALIZEPROC>("nvnCommandBufferInitialize");
    auto begin_rec = abi::proc<PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC>("nvnCommandBufferBeginRecording");
    auto end_rec = abi::proc<PFNNVNCOMMANDBUFFERENDRECORDINGPROC>("nvnCommandBufferEndRecording");
    auto draw = abi::proc<PFNNVNCOMMANDBUFFERDRAWARRAYSINSTANCEDPROC>("nvnCommandBufferDrawArraysInstanced");
    auto draw_e = abi::proc<PFNNVNCOMMANDBUFFERDRAWELEMENTSINSTANCEDPROC>("nvnCommandBufferDrawElementsInstanced");
    NVNcommandBuffer c;
    std::memset(&c, 0, sizeof c);
    CHECK(init(&c, nullptr));
    begin_rec(&c);
    draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 3, 30, 5, 7);
    draw_e(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 24, 0x7200008000ull, -4, 2, 9);
    Ref<CmdList> list = handle_lookup(end_rec(&c));
    REQUIRE(static_cast<bool>(list));
    RecordReader r(*list);
    REQUIRE(r.next());
    const P_DrawArrays pa = r.payload<P_DrawArrays>();
    REQUIRE(r.next());
    const P_DrawElements pe = r.payload<P_DrawElements>();

    dc::StreamBuilder b;
    b.draw_arrays_instanced(pa.mode, pa.first, pa.count, pa.base_instance, pa.instance_count)
        .draw_elements_instanced(pe.mode, pe.type, pe.count, pe.index_address, pe.base_vertex, pe.base_instance, pe.instance_count);
    const auto d = decode_all(b);
    REQUIRE(d.size() == 2u);
    CHECK_EQ(d[0].mode, pa.mode); CHECK_EQ(d[0].first, pa.first); CHECK_EQ(d[0].count, pa.count);
    CHECK_EQ(d[0].base_instance, pa.base_instance); CHECK_EQ(d[0].instance_count, pa.instance_count);
    CHECK_EQ(d[1].mode, pe.mode); CHECK_EQ(d[1].index_type, pe.type); CHECK_EQ(d[1].count, pe.count); CHECK_EQ(d[1].index_address, pe.index_address);
    CHECK_EQ(d[1].base_vertex, pe.base_vertex); CHECK_EQ(d[1].base_instance, pe.base_instance); CHECK_EQ(d[1].instance_count, pe.instance_count);
}

NXTEST(binds_and_state_packets_decode) {
    dc::StreamBuilder b;
    // golden words from the sdk's writers: stencil ref (0x800004E5 front / 0x800003D5 back with the value in bits 16..23), restart (0x80010591, 0x20010592)
    b.set_stencil(0, 3, 0x7F);
    REQUIRE(b.words().size() == 2u);
    CHECK_EQ(b.words()[0], 0x807F04E5u);
    CHECK_EQ(b.words()[1], 0x807F03D5u);
    b.set_stencil(1, 1, 0xFF).set_stencil(2, 2, 0x0F);
    b.set_primitive_restart(true, 0xFFFF).set_primitive_restart(false, 0);
    CHECK_EQ(b.words()[4], 0x80010591u);
    CHECK_EQ(b.words()[5], 0x20010592u);
    CHECK_EQ(b.words()[6], 0xFFFFu);
    CHECK_EQ(b.words()[7], 0x80000591u);
    b.set_scissor(2, 10, 20, 300, 200).set_scissor(0, 0, 0, 0, 0);
    // scissor 2: INC, 2 words, method 0x381 + 8; x 10..310, y 20..220
    CHECK_EQ(b.words()[8], dc::make_header(dc::kSecInc, 2, 0, 0x389));
    CHECK_EQ(b.words()[9], 10u | (310u << 16));
    CHECK_EQ(b.words()[10], 20u | (220u << 16));
    const size_t mark = b.words().size();
    b.bind_vertex_buffer(3, 0x7200001000ull, 0x2000);
    const uint64_t two[4] = {0x7200010000ull, 0x400, 0x7200020000ull, 0x800};
    b.bind_vertex_buffers(5, 2, two);
    CHECK_EQ(b.words()[mark + 6], 0xA0090E98u);      // 2 buffers: count field 4 * 2 + 1
    CHECK_EQ(b.words()[mark + 7], (1u << 16) | 5u);
    b.bind_uniform_buffer(4, 1, 0x7200030000ull, 0x10000).bind_uniform_buffer(0, 2, 0, 0);
    const size_t u = b.words().size() - 8;
    CHECK_EQ(b.words()[u], 0xA0040E02u);
    CHECK_EQ(b.words()[u + 1], 10u);                 // fragment stage value
    CHECK_EQ(b.words()[u + 4], (256u << 20) | 0x72u); // 64 KiB in 256s, address bits 32..39
    CHECK_EQ(b.words()[u + 5], 0xA0020E00u);

    dc::Stats st;
    const auto c = decode_all(b, &st);
    CHECK_EQ(st.bad_args, 0u);
    // stencil: ref front, ref back, value mask front, write mask back
    REQUIRE(c.size() == 14u);   // 4 stencil, 3 restart, 2 scissor, 3 vertex buffer, 2 uniform buffer
    CHECK(c[0].kind == dc::Kind::SetStencil && c[0].what == 0 && c[0].face == 1 && c[0].value == 0x7F);
    CHECK(c[1].kind == dc::Kind::SetStencil && c[1].what == 0 && c[1].face == 2);
    CHECK(c[2].what == 1 && c[2].face == 1 && c[2].value == 0xFF);
    CHECK(c[3].what == 2 && c[3].face == 2 && c[3].value == 0x0F);
    // restart: enable (index not known yet), index, disable
    CHECK(c[4].kind == dc::Kind::SetPrimitiveRestart && c[4].enable == 1);
    CHECK(c[5].kind == dc::Kind::SetPrimitiveRestart && c[5].enable == 1 && c[5].value == 0xFFFF);
    CHECK(c[6].kind == dc::Kind::SetPrimitiveRestart && c[6].enable == 0);
    CHECK(c[7].kind == dc::Kind::SetScissor && c[7].slot == 2 && c[7].rect[0] == 10 && c[7].rect[1] == 20 && c[7].rect[2] == 300 && c[7].rect[3] == 200);
    CHECK(c[8].kind == dc::Kind::SetScissor && c[8].slot == 0 && c[8].rect[2] == 0 && c[8].rect[3] == 0);
    CHECK(c[9].kind == dc::Kind::BindVertexBuffer && c[9].slot == 3 && c[9].address == 0x7200001000ull && c[9].size == 0x2000);
    CHECK(c[10].kind == dc::Kind::BindVertexBuffer && c[10].slot == 5 && c[10].address == 0x7200010000ull && c[10].size == 0x400);
    CHECK(c[11].kind == dc::Kind::BindVertexBuffer && c[11].slot == 6 && c[11].address == 0x7200020000ull && c[11].size == 0x800);
    CHECK(c[12].kind == dc::Kind::BindUniformBuffer && c[12].stage == 0 + 4 && c[12].slot == 1 && c[12].address == 0x7200030000ull && c[12].size == 0x10000);
    CHECK(c[13].kind == dc::Kind::BindUniformBuffer && c[13].stage == 0 && c[13].slot == 2 && c[13].size == 0);
}
