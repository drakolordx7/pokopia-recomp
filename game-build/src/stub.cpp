// SPDX-License-Identifier: GPL-3.0-or-later
// The game exe of release model (b): built on the player's PC (tools/build/game_build.py, bundled Zig) from this file and the libraries
// lifted out of their own dump. All it does is hand the lifted module tables to the runtime DLL (rt_api.h). The module list comes from
// the game manifest through game_manifest.h, which the build writes next to it.
#include <stdint.h>
#include "game_manifest.h"
#include "rt_api.h"

extern "C" {
#define X(name, file, base) extern const struct NxModule nxmod_##name; extern const uint32_t nxmod_##name##_abi;
GAME_MODULES(X)
#undef X
}

// The manifest (game.manifest, embedded by game.rc) sets the UTF-8 code page, so argv is UTF-8 like the runtime expects.
int main(int argc, char** argv) {
    static const struct NxModule* const mods[] = {
#define X(name, file, base) &nxmod_##name,
        GAME_MODULES(X)
#undef X
    };
    static const uint32_t* const abis[] = {
#define X(name, file, base) &nxmod_##name##_abi,
        GAME_MODULES(X)
#undef X
    };
    static const GameRtModules table = {
        sizeof(GameRtModules), GAME_RT_API, sizeof(mods) / sizeof(mods[0]), mods, abis,
    };
    return game_rt_main(argc, argv, &table);
}
