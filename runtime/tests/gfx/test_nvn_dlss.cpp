// SPDX-License-Identifier: GPL-3.0-or-later
// NVN2 DLSS (src/gfx/nvn/api_dlss.cpp): every nvnDlss* function is called through its thunk the way the game calls it, the builder / network / parameters
// layouts round-trip, and nvnCommandBufferEvaluateDlss is recorded and replayed: with no temporal upscaler selected a 4x4 image is scaled to 8x8 by the
// plain path and the output size and pixels are read back.
#include <cstdint>
#include <cstring>
#include <vector>

#include "nvn_app.h"
#include "nvn_impl.h"
NXTEST_GROUP(gfx_nvn_dlss)

using namespace nxrt;
using namespace nxrt::nvn;
using nvnapp::App;

namespace {

template <class Sig>
abi::Caller<Sig> proc(const char* name) {
    return abi::Caller<Sig>(nxrt::nvn::proc_address(name));
}

// The three objects as the game holds them: zero-filled, 8-byte aligned storage of the sizes the sdk uses (docs/nvn2_abi.md).
struct Objects {
    std::vector<uint64_t> builder = std::vector<uint64_t>(0xB8 / 8, 0xABABABABABABABABull);
    std::vector<uint64_t> network = std::vector<uint64_t>(0x220 / 8, 0);
    std::vector<uint64_t> params = std::vector<uint64_t>(0xC0 / 8, 0xABABABABABABABABull);
    void* b() { return builder.data(); }
    void* n() { return network.data(); }
    void* p() { return params.data(); }
};

void setup() {
    install();
    abi::reset_mismatches();
    register_all_impls();
}

}  // namespace

NXTEST(builder_network_and_parameters_round_trip) {
    setup();
    Objects o;
    proc<void(void*)>("nvnDlssNetworkBuilderSetDefaults")(o.b());
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetMaxEvaluationEntries")(o.b()), 1u);
    int dev_dummy = 0;
    proc<void(void*, void*)>("nvnDlssNetworkBuilderSetDevice")(o.b(), &dev_dummy);
    CHECK_EQ(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetDevice")(o.b()), reinterpret_cast<uint64_t>(&dev_dummy));
    proc<void(void*, uint32_t)>("nvnDlssNetworkBuilderSetFlags")(o.b(), 0x4Eu);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetFlags")(o.b()), 0x4Eu);
    proc<void(void*, int)>("nvnDlssNetworkBuilderSetMethod")(o.b(), 1);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetMethod")(o.b()), 1u);
    proc<void(void*, int)>("nvnDlssNetworkBuilderSetPreset")(o.b(), 9);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetPreset")(o.b()), 9u);
    proc<void(void*, int, int)>("nvnDlssNetworkBuilderSetOutputResolution")(o.b(), 1920, 1080);
    int w = 0, h = 0;
    proc<void(const void*, int*, int*)>("nvnDlssNetworkBuilderGetOutputResolution")(o.b(), &w, &h);
    CHECK_EQ(w, 1920);
    CHECK_EQ(h, 1080);
    char storage[16];
    proc<void(void*, void*, size_t)>("nvnDlssNetworkBuilderSetStorage")(o.b(), storage, 0x40000);
    CHECK_EQ(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetMemory")(o.b()), reinterpret_cast<uint64_t>(storage));
    CHECK_EQ(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetMemorySize")(o.b()), 0x40000ull);
    proc<void(void*, int)>("nvnDlssNetworkBuilderSetMaxEvaluationEntries")(o.b(), 3);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetMaxEvaluationEntries")(o.b()), 3u);
    int ids[32], back[32];
    for (int i = 0; i < 32; ++i) ids[i] = 100 + i;
    int pool_dummy = 0;
    proc<void(void*, void*, const int*)>("nvnDlssNetworkBuilderSetTextureIDs")(o.b(), &pool_dummy, ids);
    proc<void(const void*, int*)>("nvnDlssNetworkBuilderGetTextureIDs")(o.b(), back);
    CHECK_EQ(std::memcmp(ids, back, sizeof ids), 0);
    CHECK_EQ(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetTexturePool")(o.b()), reinterpret_cast<uint64_t>(&pool_dummy));
    CHECK(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetStorageSize")(o.b()) >= 0x1000);

    // with no temporal upscaler selected the optimal render size is the output size, as one size (largest = smallest)
    int ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0;
    CHECK(proc<bool(const void*, int*, int*, int*, int*, int*, int*)>("nvnDlssNetworkBuilderGetOptimalSettings")(o.b(), &ow, &oh, &xw, &xh, &nw, &nh));
    CHECK_EQ(ow, 1920);
    CHECK_EQ(oh, 1080);
    CHECK(xw == ow && xh == oh && nw == ow && nh == oh);

    // the network
    CHECK(proc<bool(void*, const void*)>("nvnDlssNetworkInitialize")(o.n(), o.b()));
    CHECK(proc<bool(void*, int, int)>("nvnDlssNetworkResize")(o.n(), 1280, 720));
    CHECK(!proc<bool(void*, int, int)>("nvnDlssNetworkResize")(o.n(), 2560, 1440));   // beyond the size it was made for
    proc<void(void*)>("nvnDlssNetworkFinalize")(o.n());
    CHECK(!proc<bool(void*, int, int)>("nvnDlssNetworkResize")(o.n(), 1280, 720));
    int zero_builder_ok = proc<bool(void*, const void*)>("nvnDlssNetworkInitialize")(o.n(), [] {
        static std::vector<uint64_t> empty(0xB8 / 8, 0);
        return empty.data();
    }());
    CHECK_EQ(zero_builder_ok, 0);   // no output resolution: no network

    // the parameters
    void* p = o.p();
    proc<void(void*)>("nvnDlssParametersSetDefaults")(p);
    float jx = 9, jy = 9, mx = 0, my = 0;
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetMotionVectorScale")(p, &mx, &my);
    CHECK(mx == 1.0f && my == 1.0f);
    CHECK_EQ(proc<float(const void*)>("nvnDlssParametersGetPreExposure")(p), 1.0f);
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetJitterOffset")(p, &jx, &jy);
    CHECK(jx == 0.0f && jy == 0.0f);
    proc<void(void*, float, float)>("nvnDlssParametersSetJitterOffset")(p, 0.25f, -0.5f);
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetJitterOffset")(p, &jx, &jy);
    CHECK(jx == 0.25f && jy == -0.5f);
    proc<void(void*, float, float)>("nvnDlssParametersSetMotionVectorScale")(p, 2.0f, 3.0f);
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetMotionVectorScale")(p, &mx, &my);
    CHECK(mx == 2.0f && my == 3.0f);
    proc<void(void*, float)>("nvnDlssParametersSetPreExposure")(p, 0.5f);
    CHECK_EQ(proc<float(const void*)>("nvnDlssParametersGetPreExposure")(p), 0.5f);
    proc<void(void*, int)>("nvnDlssParametersSetHistoryReset")(p, 1);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssParametersGetHistoryReset")(p), 1u);
    proc<void(void*, int)>("nvnDlssParametersSetEvaluationIndex")(p, 2);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssParametersGetEvaluationIndex")(p), 2u);
    struct Pair {
        const char* set;
        const char* get;
    };
    const Pair pairs[] = {{"nvnDlssParametersSetInputColorSubrectangleBase", "nvnDlssParametersGetInputColorSubrectangleBase"},
                          {"nvnDlssParametersSetDepthSubrectangleBase", "nvnDlssParametersGetDepthSubrectangleBase"},
                          {"nvnDlssParametersSetMotionVectorSubrectangleBase", "nvnDlssParametersGetMotionVectorSubrectangleBase"},
                          {"nvnDlssParametersSetBiasCurrentColorSubrectangleBase", "nvnDlssParametersGetBiasCurrentColorSubrectangleBase"},
                          {"nvnDlssParametersSetOutputSubrectangleBase", "nvnDlssParametersGetOutputSubrectangleBase"},
                          {"nvnDlssParametersSetInputSubrectangleSize", "nvnDlssParametersGetInputSubrectangleSize"}};
    int k = 10;
    for (const Pair& pr : pairs) {
        proc<void(void*, int, int)>(pr.set)(p, k, k + 1);
        int a = 0, b = 0;
        proc<void(const void*, int*, int*)>(pr.get)(p, &a, &b);
        CHECK_MSG(a == k && b == k + 1, pr.get);
        k += 10;
    }
    // the texture pairs and the overlay switches are stored (nothing to read back but the neighbours stay intact)
    int tex_dummy = 0;
    for (const char* n : {"nvnDlssParametersSetInputColor", "nvnDlssParametersSetOutputColor", "nvnDlssParametersSetMotionVectors", "nvnDlssParametersSetDepth",
                          "nvnDlssParametersSetBiasCurrentColorMask", "nvnDlssParametersSetExposureTexture"})
        proc<void(void*, const void*, const void*)>(n)(p, &tex_dummy, nullptr);
    for (const char* n : {"nvnDlssParametersSetOverlayDebugLayer", "nvnDlssParametersSetOverlayFullScreen", "nvnDlssParametersSetOverlayShowNans",
                          "nvnDlssParametersSetOverlayJitterDebug"})
        proc<void(void*, int)>(n)(p, 0);
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetJitterOffset")(p, &jx, &jy);
    CHECK(jx == 0.25f && jy == -0.5f);
    CHECK_EQ(abi::mismatches(), 0u);
}

// The plain path: no temporal mode is selected ([upscale] mode off), so nvnCommandBufferEvaluateDlss scales the input rectangle into the output
// texture with the bilinear filter. A 4x4 picture (left half red, right half blue) becomes 8x8.
NXTEST(evaluate_dlss_scales_a_small_image_through_the_plain_path) {
    App app;
    App::Opts o;
    o.width = 64;
    o.height = 48;
    REQUIRE(app.init(o));
    auto src = app.make_texture_2d(NVN_FORMAT_RGBA8, 4, 4);
    auto dst = app.make_texture_2d(NVN_FORMAT_RGBA8, 8, 8);
    REQUIRE(src != nullptr && dst != nullptr);

    uint8_t pic[4 * 4 * 4];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* px = pic + (y * 4 + x) * 4;
            px[0] = x < 2 ? 255 : 0;
            px[1] = 0;
            px[2] = x < 2 ? 0 : 255;
            px[3] = 255;
        }
    App::Buf up = app.make_buffer(sizeof pic, pic);
    App::Buf back = app.make_buffer(8 * 8 * 4);
    std::memset(back.ptr, 0xEE, 8 * 8 * 4);

    Objects obj;
    proc<void(void*)>("nvnDlssNetworkBuilderSetDefaults")(obj.b());
    proc<void(void*, void*)>("nvnDlssNetworkBuilderSetDevice")(obj.b(), app.device.get());
    proc<void(void*, int, int)>("nvnDlssNetworkBuilderSetOutputResolution")(obj.b(), 8, 8);
    REQUIRE(proc<bool(void*, const void*)>("nvnDlssNetworkInitialize")(obj.n(), obj.b()));
    proc<void(void*)>("nvnDlssParametersSetDefaults")(obj.p());
    proc<void(void*, const void*, const void*)>("nvnDlssParametersSetInputColor")(obj.p(), src->obj.get(), nullptr);
    proc<void(void*, const void*, const void*)>("nvnDlssParametersSetOutputColor")(obj.p(), dst->obj.get(), nullptr);
    proc<void(void*, int, int)>("nvnDlssParametersSetInputSubrectangleSize")(obj.p(), 4, 4);

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    app.api.nvnCommandBufferBeginRecording(c);
    app.api.nvnCommandBufferSetCopyRowStride(c, 16);
    NVNcopyRegion up_region{0, 0, 0, 4, 4, 1};
    app.api.nvnCommandBufferCopyBufferToTexture(c, up.address, src->obj.get(), nullptr, &up_region, NVN_COPY_FLAGS_NONE);
    proc<void(NVNcommandBuffer*, const void*, const void*)>("nvnCommandBufferEvaluateDlss")(c, obj.n(), obj.p());
    app.api.nvnCommandBufferSetCopyRowStride(c, 32);
    NVNcopyRegion down_region{0, 0, 0, 8, 8, 1};
    app.api.nvnCommandBufferCopyTextureToBuffer(c, dst->obj.get(), nullptr, &down_region, back.address, NVN_COPY_FLAGS_NONE);
    app.end_and_submit(*cb);
    app.finish();

    // output size: every one of the 8 x 8 pixels was written (the sentinel is gone), and the picture is the enlarged input
    size_t untouched = 0, wrong = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const uint8_t* px = back.ptr + (y * 8 + x) * 4;
            if (px[0] == 0xEE && px[1] == 0xEE && px[2] == 0xEE && px[3] == 0xEE) ++untouched;
            if (x <= 2 && !(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255)) ++wrong;   // only red source texels reach these
            if (x >= 5 && !(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255)) ++wrong;   // only blue ones reach these
        }
    std::printf("    pixel (0,0) %d %d %d %d, pixel (7,7) %d %d %d %d, %zu unwritten, %zu wrong\n", back.ptr[0], back.ptr[1], back.ptr[2], back.ptr[3],
                back.ptr[(7 * 8 + 7) * 4], back.ptr[(7 * 8 + 7) * 4 + 1], back.ptr[(7 * 8 + 7) * 4 + 2], back.ptr[(7 * 8 + 7) * 4 + 3], untouched, wrong);
    CHECK_EQ(untouched, 0u);
    CHECK_EQ(wrong, 0u);
    const auto v = app.validation();
    CHECK_EQ(v.errors, 0ull);
    app.shutdown();
}
