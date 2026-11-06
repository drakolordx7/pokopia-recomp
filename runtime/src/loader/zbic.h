// SPDX-License-Identifier: GPL-3.0-or-later
// ZBIC: the zstd variant of NSO segments with flag bit 7 (UseZbicCompression, firmware 22.0.0+). docs/zbic.md has the format.
// Plain C++ interface, no runtime dependencies, so tools/extract compiles this file as well.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt {

constexpr uint32_t kZbicMagic = 0x4349425A;          // "ZBIC" little-endian: the frame magic in place of zstd's 0xFD2FB528
constexpr uint32_t kNsoFlagZbic = 1u << 7;           // NSO0 header +0x0C

// True when src starts with a ZBIC frame magic.
bool zbic_is_frame(const void* src, size_t n);
// Decompresses one segment (one or more ZBIC frames) into dst. True only when it decodes cleanly to exactly dst_size bytes.
bool zbic_decompress(void* dst, size_t dst_size, const void* src, size_t src_size);

}  // namespace nxrt
