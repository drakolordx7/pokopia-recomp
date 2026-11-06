# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-game configuration of Pokémon Pokopia (Nintendo Switch 2).

Identity, build id, version and the module list (names, load addresses) come from game.toml in this folder, the manifest every
build, release and launcher tool reads. What stays here is lifter knowledge that is not manifest data: the extra function entries.
Supported builds of main are listed in BUILDS (build id -> display version); the setup tool checks the build id. The lifter itself
is build-neutral: hook sites come from sites.sig, resolved per build into main.sites.
"""
import os
import sys
import tomllib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from nxrecomp.config import GameConfig, ModuleSpec          # noqa: E402

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'game.toml'), 'rb') as _f:
    MANIFEST = tomllib.load(_f)
_GAME = MANIFEST['game']
_BUILD_ID = MANIFEST['dump']['main_build_id'].upper()      # of main (first 20 bytes; the rest of the 32-byte field is zero)

BUILDS = {
    _BUILD_ID: _GAME['version'],
}


class Config(GameConfig):
    name = _GAME['id']
    title_id = _GAME['title_id']
    version = _GAME['version']
    build_id = _BUILD_ID
    modules = [ModuleSpec(m['name'], m['base'], m['file']) for m in MANIFEST['module']]
    overrides = {}
    # rtld's process entry (0x0) has no FDE, symbol or direct branch, so discovery never roots it (runtime/docs/S6.md)
    extra_entries = {'rtld': [0x0]}
    patches = None
