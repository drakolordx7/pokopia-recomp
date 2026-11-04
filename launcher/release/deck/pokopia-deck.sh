#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs pokopia-setup.exe through Proton from a terminal (Steam Deck Desktop Mode, or any Linux PC with Steam), so the setup's
# progress is visible while it runs. Put this script in the game folder, next to pokopia-setup.exe and PokopiaLauncher.exe.
#
#   ./pokopia-deck.sh setup /run/media/deck/<card>/pokopia/dump     build pkp.exe and the shader cache (resumable: run it again to continue)
#   ./pokopia-deck.sh status                                        what is done
#   ./pokopia-deck.sh preset deck                                   controls.ini: borderless window (do this once before the first Play)
#   ./pokopia-deck.sh play                                          start the game through Proton (for Game Mode, add pkp.exe to Steam instead)
#   ./pokopia-deck.sh pause                                         ask a running setup to stop at the next safe point
#
# Linux paths in the arguments (anything starting with /) are passed to Windows as Z:/... .
# Environment: PROTON=<path to a 'proton' script> picks the Proton build (default: Proton Experimental, then GE-Proton, then any Proton);
# POKOPIA_PREFIX=<dir> is the Wine prefix (default: <game folder>/proton-prefix; it only holds Proton's own files, nothing of the game).
set -euo pipefail

GAME_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE="$GAME_DIR/pokopia-setup.exe"
if [ ! -f "$EXE" ]; then
    echo "pokopia-setup.exe was not found next to this script ($GAME_DIR)." >&2
    exit 1
fi

if [ "${1:-}" = "pause" ]; then
    touch "$GAME_DIR/setup.cancel"
    echo "Pause requested: the setup stops at the next safe point. Run 'setup' again to continue."
    exit 0
fi

find_steam() {
    local d
    for d in "${STEAM_ROOT:-}" "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root"; do
        if [ -n "$d" ] && [ -d "$d/steamapps" ]; then
            echo "$d"
            return 0
        fi
    done
    return 1
}

STEAM="$(find_steam || true)"
if [ -z "$STEAM" ]; then
    echo "Steam was not found (looked in ~/.local/share/Steam and ~/.steam). Set STEAM_ROOT=<Steam folder>." >&2
    exit 1
fi

find_proton() {
    if [ -n "${PROTON:-}" ]; then
        echo "$PROTON"
        return 0
    fi
    local c
    # Proton Experimental first, in the main library or on an SD card library
    for c in "$STEAM/steamapps/common/Proton - Experimental/proton" /run/media/*/steamapps/common/"Proton - Experimental"/proton \
             /run/media/*/*/steamapps/common/"Proton - Experimental"/proton; do
        [ -f "$c" ] && { echo "$c"; return 0; }
    done
    # then the newest GE-Proton
    c="$(ls -d "$STEAM/compatibilitytools.d"/GE-Proton*/proton "$HOME/.steam/root/compatibilitytools.d"/GE-Proton*/proton 2>/dev/null | sort -V | tail -1 || true)"
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return 0; }
    # then any Proton
    c="$(ls -d "$STEAM/steamapps/common"/Proton*/proton /run/media/*/steamapps/common/Proton*/proton /run/media/*/*/steamapps/common/Proton*/proton 2>/dev/null | sort -V | tail -1 || true)"
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return 0; }
    return 1
}

PROTON_BIN="$(find_proton || true)"
if [ -z "$PROTON_BIN" ]; then
    echo "No Proton found. Install Proton Experimental from Steam (Library, Tools), or set PROTON=<path to its 'proton' script>." >&2
    exit 1
fi

export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM"
export STEAM_COMPAT_DATA_PATH="${POKOPIA_PREFIX:-$GAME_DIR/proton-prefix}"
mkdir -p "$STEAM_COMPAT_DATA_PATH"

# Linux paths become Z: paths for the Windows side
args=()
for a in "$@"; do
    case "$a" in
        /*) args+=("Z:$a") ;;
        *) args+=("$a") ;;
    esac
done

echo "Proton: $PROTON_BIN"
echo "Prefix: $STEAM_COMPAT_DATA_PATH"
echo "Running: pokopia-setup.exe ${args[*]:-}"
cd "$GAME_DIR"
exec "$PROTON_BIN" run "$EXE" "${args[@]}"
