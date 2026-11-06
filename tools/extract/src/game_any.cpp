// SPDX-License-Identifier: GPL-3.0-or-later
// nx_extract is game independent: the title id comes from the command line (the launcher passes game.toml's [game] title_id). The
// setup sources it reuses still ask game() for names in a few messages, so this profile gives neutral ones. A non-empty build list
// makes them treat the dump as "base game + update allowed".
#include "game.h"

namespace mprs {

const GameProfile& game() {
    static const GameBuild builds[] = {{"0", ""}, {nullptr, nullptr}};
    static const GameProfile g{
        "any", "the game", "nx_extract", "", "0", "", "", "", builds,
    };
    return g;
}

}  // namespace mprs
