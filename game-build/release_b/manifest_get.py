# SPDX-License-Identifier: GPL-3.0-or-later
"""Prints values of a game manifest (game.toml) for shell scripts (.weaver/release_b.sh).

    python -I manifest_get.py <game.toml> <table.key> [<table.key> ...]      one value per line, in the order asked
    python -I manifest_get.py <game.toml> --env                              shell assignments (GM_<TABLE>_<KEY>=...) of every scalar value

Dotted keys address tables: game.id, files.exe, dump.display_version, launcher.product_name, shaders.extractor_script.
"""
import shlex
import sys
import tomllib


def main(argv):
    sys.stdout.reconfigure(encoding='utf-8')
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1], 'rb') as f:
        m = tomllib.load(f)
    if argv[2] == '--env':
        for table, body in m.items():
            if isinstance(body, dict):
                for k, v in body.items():
                    if isinstance(v, (str, int, bool)):
                        print('GM_%s_%s=%s' % (table.upper(), k.upper(), shlex.quote(str(v))))
        return 0
    for key in argv[2:]:
        v = m
        for part in key.split('.'):
            v = v[part]
        print(v)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
