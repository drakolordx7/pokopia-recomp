// SPDX-License-Identifier: GPL-3.0-or-later
// The one call between the game exe (linked on the player's PC: the lifted game libraries + stub.cpp) and the runtime DLL (shipped
// prebuilt). Plain C, so the two sides may come from different compilers (zig/clang mingw for the exe, clang-cl for the DLL).
// Everything else the lifted code calls (recomp ctx.h: call_indirect, host_import, guest_svc, ...) is exported by the DLL as well
// (src/rt.def.in).
#pragma once
#include <stdint.h>

#define GAME_RT_API 2u     // bump when this struct or the exported set changes

#ifdef __cplusplus
extern "C" {
#endif
struct NxModule;
typedef struct GameRtModules {
    uint32_t size;        // sizeof(GameRtModules)
    uint32_t api;         // GAME_RT_API
    uint32_t count;       // number of modules: the manifest's [[module]] list, entry module first (GAME_MODULES)
    const struct NxModule* const* mods;
    const uint32_t* const* abis;
} GameRtModules;

int game_rt_main(int argc, char** argv, const GameRtModules* mods);
#ifdef __cplusplus
}
#endif
