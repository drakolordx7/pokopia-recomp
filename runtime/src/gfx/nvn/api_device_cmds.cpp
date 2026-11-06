// SPDX-License-Identifier: GPL-3.0-or-later
// NVN2 device command buffers (family device_cmds): memory the GPU fills with command words, which a command buffer then runs.
//   nvnDeviceCommandBuffer{SetDefaults, SetDevice, SetStorage, SetCommandMemorySize, SetComputeMemorySize, GetHandle}
//   nvnCommandBuffer{Begin, End, Submit}DeviceCommands
// The object is the guest's 0x30 bytes, laid out as the sdk keeps it (docs/nvn2_device_commands.md):
//   +0x00 device, +0x08 memory pool, +0x10 offset in the pool, +0x18 storage size, +0x20 command memory size, +0x28 compute memory size.
// Begin and End bracket the GPU's work on the storage (the console's GPU sets up and closes the header there); the replay needs neither of them
// beyond checking they pair. Submit records one SubmitDeviceCommands op; the replay reads the command words from the storage, decodes them
// (nvn_devcmd.h) and runs the draws in them (nvn_replay.cpp). Where VK_EXT_device_generated_commands would replace that read-back: the doc.
#include <atomic>
#include <cstring>
#include <mutex>

#include "nvn_cmd.h"
#include "nvn_devcmd.h"
#include "nvn_impl.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
using namespace be;
namespace {

struct DevCmdBuf {            // the sdk's NVNdeviceCommandBuffer (0x30 bytes)
    uint64_t device;
    uint64_t pool;
    int64_t offset;
    int64_t size;
    int64_t command_size;
    int64_t compute_size;
};
static_assert(sizeof(DevCmdBuf) == 0x30, "NVNdeviceCommandBuffer is 0x30 bytes");

DevCmdBuf* dcb_of(void* p) { return static_cast<DevCmdBuf*>(p); }

// GPU address of byte 0 of the storage (the sdk's GetHandle): the pool's address plus the offset, 0 without a pool.
uint64_t storage_address(const DevCmdBuf* d) {
    const MemoryPool* mp = host_of<MemoryPool>(reinterpret_cast<const void*>(d->pool));
    if (!d->pool || !mp) return 0;
    return mp->gpu_base + static_cast<uint64_t>(d->offset);
}

void nvnDeviceCommandBufferSetDefaults(void* p) {
    if (!p) return;
    DevCmdBuf* d = dcb_of(p);
    d->pool = 0;
    d->offset = d->size = d->command_size = d->compute_size = 0;   // the sdk clears +8 .. +0x30 and keeps the device
}
void nvnDeviceCommandBufferSetDevice(void* p, void* device) {
    if (p) dcb_of(p)->device = reinterpret_cast<uint64_t>(device);
}
void nvnDeviceCommandBufferSetStorage(void* p, void* pool, int64_t offset, int64_t size) {
    if (!p) return;
    DevCmdBuf* d = dcb_of(p);
    d->pool = reinterpret_cast<uint64_t>(pool);
    d->offset = offset;
    d->size = size;
}
void nvnDeviceCommandBufferSetCommandMemorySize(void* p, int64_t size) { if (p) dcb_of(p)->command_size = size; }
void nvnDeviceCommandBufferSetComputeMemorySize(void* p, int64_t size) { if (p) dcb_of(p)->compute_size = size; }
uint64_t nvnDeviceCommandBufferGetHandle(void* p) { return p ? storage_address(dcb_of(p)) : 0; }

CommandBuffer* recording_buffer(NVNcommandBuffer* cmd, std::unique_lock<std::mutex>& lk) {
    CommandBuffer* cb = host_of<CommandBuffer>(cmd);
    if (!cb) return nullptr;
    lk = std::unique_lock<std::mutex>(cb->lock);
    return (cb->recording && cb->rec) ? cb : nullptr;
}

void nvnCommandBufferBeginDeviceCommands(NVNcommandBuffer* cmd, void* p) {
    if (!p) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = recording_buffer(cmd, lk);
    if (!cb) return;
    const uint64_t base = storage_address(dcb_of(p));
    if (cb->device_cmds_open)
        NXLOG_W("nvn", "nvnCommandBufferBeginDeviceCommands(0x%llx): the commands of 0x%llx are not ended", static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(cb->device_cmds_open));
    cb->device_cmds_open = base ? base : 1;
}

void nvnCommandBufferEndDeviceCommands(NVNcommandBuffer* cmd, void* p) {
    if (!p) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = recording_buffer(cmd, lk);
    if (!cb) return;
    const uint64_t base = storage_address(dcb_of(p));
    if (!cb->device_cmds_open || (base && cb->device_cmds_open != base))
        NXLOG_W("nvn", "nvnCommandBufferEndDeviceCommands(0x%llx): no matching Begin (open: 0x%llx)", static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(cb->device_cmds_open));
    cb->device_cmds_open = 0;
}

void nvnCommandBufferSubmitDeviceCommands(NVNcommandBuffer* cmd, void* p) {
    if (!p) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = recording_buffer(cmd, lk);
    if (!cb) return;
    const DevCmdBuf* d = dcb_of(p);
    const uint64_t base = storage_address(d);
    static std::atomic<bool> noted{false};
    if (!base) {
        if (!noted.exchange(true)) NXLOG_W("nvn", "nvnCommandBufferSubmitDeviceCommands: the device command buffer has no storage; nothing recorded");
        return;
    }
    P_SubmitDeviceCommands s{};
    s.stream_address = base + devcmd::kHeaderBytes;
    s.stream_size = d->command_size > 0 ? static_cast<uint64_t>(d->command_size) : 0;
    // The compute memory starts at the next 256-byte boundary after the command words (the sdk's Begin writes (command size + 511) & ~255).
    s.compute_address = d->compute_size > 0 ? base + ((static_cast<uint64_t>(d->command_size) + 511u) & ~255ull) : 0;
    s.compute_size = d->compute_size > 0 ? static_cast<uint64_t>(d->compute_size) : 0;
    cb->rec->emit(Op::SubmitDeviceCommands, s);
    cb->rec->draw_count++;
}

}  // namespace

void register_api_device_cmds() {
    NVN2_REG(nvnDeviceCommandBufferSetDefaults);
    NVN2_REG(nvnDeviceCommandBufferSetDevice);
    NVN2_REG(nvnDeviceCommandBufferSetStorage);
    NVN2_REG(nvnDeviceCommandBufferSetCommandMemorySize);
    NVN2_REG(nvnDeviceCommandBufferSetComputeMemorySize);
    NVN2_REG(nvnDeviceCommandBufferGetHandle);
    NVN2_REG(nvnCommandBufferBeginDeviceCommands);
    NVN2_REG(nvnCommandBufferEndDeviceCommands);
    NVN2_REG(nvnCommandBufferSubmitDeviceCommands);
}

}  // namespace nxrt::nvn
