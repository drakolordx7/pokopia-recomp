// SPDX-License-Identifier: GPL-3.0-or-later
// Pokopia, the mouse in the game's menus (pkp_menu.h): the shared KTGL adapter (src/gfx/play/menu_ktgl.cpp) with the offsets of the 2.0.1
// update's main, found in IDA (work/re/menus.md). The engine (hit-test, hover, click, the gate) is src/gfx/play/menu_pointer.cpp.
#include "pkp_menu.h"
#include "pkp_play.h"

#include "menu_ktgl.h"

namespace nxrt::games::pkp {

namespace {

// Every KTGL pane class's draw (vtable slot +0x2b0; e.g. the picture pane 0x7101f5b248) ORs 0x20 into the pane's flags first: these are
// those flag stores (pane in x0, x21 for 0x7101f77fe0), listed as midhooks in recomp/games/pkp/main.sites.
constexpr play::ktgl::DrawSite kDrawSites[] = {
    {0x1f5ac6c, 0}, {0x1f5b26c, 0}, {0x1f63094, 0}, {0x1f642a8, 0}, {0x1f6a028, 0}, {0x1f6a8c4, 0},
    {0x1f76168, 0}, {0x1f765b4, 0}, {0x1f78030, 21}, {0x1f78124, 0}, {0x1f7aa2c, 0}, {0x1f7fe48, 0},
};

const play::ktgl::Config& config() {
    static const play::ktgl::Config c = [] {
        play::ktgl::Config k;
        k.tag = "pkp";
        k.section = "pkp";
        // 881468973 = the title's "(-) Settings" guide (p5m: 'Win_000_btn_key_guide_0' at 1722,1003..1920,1075; gameplay's HUD guides use the
        // layout too, hence world_live below)
        k.default_key_guides = "881468973=btn_minus";  // gitleaks:allow (a layout id, not a key)
        // [ptr] the UI manager (ctor 0x710087bd60). It holds two placeable pools of one type (ctor 0x71009fee60), at +0x84808 (A) and
        // +0x8c858 (B). Pool: +0x8 free-slot ring, +0x1030 {entry** data, u64 count} of the entries in use (cap 512), +0x8048 mutex*,
        // slots of 48 bytes at +0x2048.
        //   pool A entry (looked up by (id, sub) in 0x710087da00 / 0x7100911bc0): +8 u32 id, +12 u32 sub, +32 -> {placeable** data, u64 count}
        //     = every instance of that layout, +40 -> ring {placeable** data, u64 size, u64 cap, u64 head, u64 count} = the instances not in
        //     use (0x710087da00 pops one when a screen opens).
        //   pool B entry (0x7100912aa0, 0x71009125e0): +0 placeable*, +8 u32 id, +12 u32 sub. Seen empty at run time (p5c: 0 layouts).
        k.ui_mgr = 0x35aebf8;
        k.pool_a = 0x84808;
        k.pool_b = 0x8c858;
        k.pool_entries = 0x1030;
        k.ent_instances = 32;
        k.ent_free = 40;
        k.placeable_obj = 40;      // placeable +40 -> object, +16 -> the layout (DoPaneHitTest 0x2611d00)
        k.obj_layout = 16;
        k.lay_pane_count = 146;    // u16 pane count; pane list: node +8 next, +16 pane
        k.lay_first_node = 32;
        k.lay_sentinel = 64;
        k.pane_name = 176;
        k.pane_tag = 184;
        k.pane_parent = 208;
        k.pane_layout = 216;       // the draw reads [pane+0xd8]+0x2e0, the layout's renderer, as 0x7101f56180 does
        k.pane_world = 224;
        k.pane_size = 240;
        k.pane_flags = 260;
        k.pane_anchor = 268;
        k.pane_name_len = 272;
        k.pane_alpha = 275;
        k.canvas_w = 1920.f;
        k.canvas_h = 1080.f;
        k.draw_sites = kDrawSites;
        k.draw_site_count = sizeof kDrawSites / sizeof kDrawSites[0];
        // gameplay's HUD draws the key guide layout (Run / Bag / Focus): it counts only while the follow camera is not running (the title)
        k.world_live = &play_follow_live;
        return k;
    }();
    return c;
}

}  // namespace

void menu_install(uint64_t base) { play::ktgl::install(base, config()); }
void menu_remove() { play::ktgl::remove(); }
play::Act menu_frame(bool menu_up, const play::mm::Geometry& g) { return play::ktgl::menu_frame(menu_up, g); }
bool menu_over_value() { return play::ktgl::menu_over_value(); }
bool menu_active() { return play::ktgl::menu_active(); }
bool menu_open() { return play::ktgl::menu_open(); }
bool menu_in_game(bool in_game) { return play::ktgl::menu_in_game(in_game); }
void menu_status_text(char* buf, size_t n) { play::ktgl::status_text(buf, n); }

}  // namespace nxrt::games::pkp
