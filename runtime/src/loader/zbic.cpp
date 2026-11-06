// SPDX-License-Identifier: GPL-3.0-or-later
// ZBIC segment decompression (zbic.h). The decoder is zstd 1.5.7 with hexkyz's ZBIC changes (third_party/zstd_zbic, BSD-3-Clause),
// compiled into this one translation unit with ZSTD_ZBIC_SUPPORT on and every zstd symbol internal (static), so a plain zstd linked
// elsewhere in the same binary does not clash with it. Format notes: docs/zbic.md.
#include "zbic.h"

#include <cstring>

#define ZSTD_ZBIC_SUPPORT 1
#define ZSTDLIB_VISIBLE static
#define ZSTDLIB_HIDDEN static
#define ZSTDERRORLIB_VISIBLE static
#define ZSTDERRORLIB_HIDDEN static
#undef ZSTD_MULTITHREAD
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "../../third_party/zstd_zbic/zstddeclib.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace nxrt {

bool zbic_is_frame(const void* src, size_t n) {
    if (n < 4) return false;
    uint32_t m;
    std::memcpy(&m, src, 4);
    return m == kZbicMagic;
}

bool zbic_decompress(void* dst, size_t dst_size, const void* src, size_t src_size) {
    if (!dst || !src || !src_size || !zbic_is_frame(src, src_size)) return false;
    const size_t r = ZSTD_decompress(dst, dst_size, src, src_size);
    return !ZSTD_isError(r) && r == dst_size;
}

}  // namespace nxrt
