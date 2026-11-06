// SPDX-License-Identifier: GPL-3.0-or-later
// The menu pointer on nn::ui2d layouts and their button controls (menu_ui2d.h). Evidence for every address: docs/menu_pointer.md.
#include "menu_ui2d.h"
#include "menu_pointer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::play::ui2d {

namespace {

const Config* g_cfg = nullptr;
const Config& C() {
    static const Config none;
    return g_cfg ? *g_cfg : none;
}

constexpr uint64_t kRecentNs = 100000000ull;   // a group whose pointer update ran in the last 100 ms is on screen
constexpr size_t kMaxGroups = 64, kMaxButtons = 256;

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }

bool ok(uint64_t a, uint64_t n) {
    if (a < 0x1000000000ull || a + n > 0x8000000000ull || n == 0 || n > 0x10000) return false;
    return mem::is_mapped(a & ~0xfffull, ((a + n + 0xfff) & ~0xfffull) - (a & ~0xfffull));
}

uint64_t rptr(uint64_t a) { return ok(a, 8) ? rd<uint64_t>(a) : 0; }

// ---- the groups the game updates (written by the hook on the game's thread, read by the main thread) ----
struct Seen { uint64_t grp = 0, vtbl = 0, ns = 0; };
std::mutex g_mu;
std::vector<Seen> g_seen;

// ---- what the hook hands the game (written by the main thread) ----
std::atomic<bool> g_feed{false};                  // the menu pointer is up: the mouse stands in for the touch
std::atomic<uint32_t> g_nx{0}, g_ny{0};           // the pointer on the UI plane (-1..1, y up), float bits
std::atomic<bool> g_held{false};
std::atomic<uint64_t> g_press_grp{0}, g_press_btn{0};
std::atomic<int> g_press_stage{0};                // 2: send the trigger, 1: send the release (a confirm on g_press_btn)
std::atomic<uint64_t> g_hook_calls{0}, g_feeds{0}, g_presses{0};
uint32_t g_hook = 0;

uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float ffrom(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// The game's own visibility rule for a hit test (Button::IsHit): the pane visible (flag bit 0) with a nonzero alpha, and every ancestor
// visible, an ancestor whose alpha reaches its children (bit 1) with a nonzero alpha.
bool pane_visible(uint64_t pane) {
    const Config& c = C();
    if (!ok(pane + c.pane_flags, 2)) return false;
    if (!(rd<uint8_t>(pane + c.pane_flags) & 1) || rd<uint8_t>(pane + c.pane_alpha) == 0) return false;
    uint64_t q = rptr(pane + c.pane_parent);
    for (int depth = 0; q && depth < 64; ++depth) {
        if (!ok(q + c.pane_flags, 2)) return false;
        const uint8_t f = rd<uint8_t>(q + c.pane_flags);
        if (!(f & 1) || ((f & 2) && rd<uint8_t>(q + c.pane_alpha) == 0)) return false;
        q = rptr(q + c.pane_parent);
    }
    return true;
}

// The hit box in layout units (centre, half sizes), as the game's IsContain builds it: the global matrix's scale and translation, the
// pane's size, its base position (horizontal bits 0-1: 0 centre, 1 left, 2 right; vertical bits 2-3: 0 centre, 1 top, 2 bottom).
bool hit_box(uint64_t pane, float* cx, float* cy, float* hw, float* hh) {
    const Config& c = C();
    if (!ok(pane, c.pane_mtx + 48)) return false;
    const float w = rd<float>(pane + c.pane_size), h = rd<float>(pane + c.pane_size + 4);
    const float m00 = rd<float>(pane + c.pane_mtx), tx = rd<float>(pane + c.pane_mtx + 12);
    const float m11 = rd<float>(pane + c.pane_mtx + 20), ty = rd<float>(pane + c.pane_mtx + 28);
    if (!std::isfinite(w) || !std::isfinite(h) || !std::isfinite(m00) || !std::isfinite(m11) || !std::isfinite(tx) || !std::isfinite(ty)) return false;
    *hw = std::fabs(w * m00) * 0.5f;
    *hh = std::fabs(h * m11) * 0.5f;
    if (*hw <= 0.f || *hh <= 0.f) return false;
    const uint8_t base = rd<uint8_t>(pane + c.pane_base);
    const int bh = base & 3, bv = (base >> 2) & 3;
    *cx = bh == 1 ? tx + *hw : (bh == 2 ? tx - *hw : tx);
    *cy = bv == 1 ? ty - *hh : (bv == 2 ? ty + *hh : ty);
    return true;
}

bool layout_size(uint64_t lay, float* w, float* h) {
    if (!ok(lay + C().lay_size, 8)) return false;
    *w = rd<float>(lay + C().lay_size);
    *h = rd<float>(lay + C().lay_size + 4);
    return std::isfinite(*w) && std::isfinite(*h) && *w > 1.f && *h > 1.f && *w < 100000.f && *h < 100000.f;
}

// The group's buttons (node -> button), at most kMaxButtons.
template <class F> void each_button(uint64_t grp, F&& f) {
    const Config& c = C();
    const uint64_t head = grp + c.grp_buttons;
    uint64_t node = rptr(head);
    for (size_t k = 0; node && node != head && k < kMaxButtons; ++k) {
        f(node - c.btn_node);
        node = rptr(node);
    }
}

// The layout of the group's first button (all of a group's buttons are in one layout).
uint64_t group_layout(uint64_t grp) {
    uint64_t lay = 0;
    each_button(grp, [&](uint64_t btn) { if (!lay) lay = rptr(btn + C().btn_layout); });
    return lay;
}

// ButtonGroup::UpdatePointer's entry (game thread): record the group; while the menu pointer is up, the mouse in place of the touch.
int hook_group_update(::Ctx* c, uint64_t, void*) {
    const uint64_t grp = c->x[0];
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    if (!grp || !ok(grp, C().grp_flags + 4)) return 0;
    {
        const uint64_t vt = rd<uint64_t>(grp), t = now_ns();
        std::lock_guard<std::mutex> lk(g_mu);
        bool found = false;
        for (Seen& s : g_seen)
            if (s.grp == grp) { s.vtbl = vt; s.ns = t; found = true; break; }
        if (!found) {
            if (g_seen.size() >= kMaxGroups) {
                auto old = std::min_element(g_seen.begin(), g_seen.end(), [](const Seen& a, const Seen& b) { return a.ns < b.ns; });
                *old = Seen{grp, vt, t};
            } else {
                g_seen.push_back(Seen{grp, vt, t});
            }
        }
    }
    if (!g_feed.load(std::memory_order_relaxed)) return 0;
    if (!(rd<uint32_t>(grp + C().grp_flags) & C().grp_pointer_bit)) return 0;
    float lw = 0, lh = 0;
    if (!layout_size(group_layout(grp), &lw, &lh)) return 0;
    thread_local float pos[2];
    pos[0] = ffrom(g_nx.load(std::memory_order_relaxed)) * lw * 0.5f;
    pos[1] = ffrom(g_ny.load(std::memory_order_relaxed)) * lh * 0.5f;
    uint64_t trig = 0, rel = 0, hold = g_held.load(std::memory_order_relaxed) ? 1 : 0;
    if (g_press_grp.load(std::memory_order_acquire) == grp) {
        const int stage = g_press_stage.load(std::memory_order_acquire);
        const uint64_t btn = g_press_btn.load(std::memory_order_relaxed);
        float cx, cy, hw, hh;
        if (stage > 0 && hit_box(rptr(btn + C().btn_hit), &cx, &cy, &hw, &hh)) {   // the press lands on the button itself
            pos[0] = cx;
            pos[1] = cy;
        }
        if (stage == 2) { trig = 1; hold = 1; g_press_stage.store(1, std::memory_order_release); g_presses.fetch_add(1, std::memory_order_relaxed); }
        else if (stage == 1) { rel = 1; hold = 0; g_press_stage.store(0, std::memory_order_release); g_press_grp.store(0, std::memory_order_release); }
    }
    c->x[1] = reinterpret_cast<uint64_t>(pos);   // guest pointers are host pointers
    c->x[2] = trig;
    c->x[3] = rel;
    c->x[4] = hold;
    c->x[5] = 1;
    g_feeds.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

struct State {
    uint64_t base = 0x7100000000ull;
    bool installed = false, enabled = true, ig_enabled = true;
    const char* section = "menu";
    mp::Gate gate;
    std::vector<uint64_t> el_group;   // this frame's elements: the group of each
    uint32_t n_groups = 0, n_buttons = 0;
    bool was_up = false;
};

State& st() {
    static State s;
    return s;
}

// The groups on screen: updated recently, still the same object (vtable unchanged).
std::vector<uint64_t> recent_groups() {
    std::vector<uint64_t> out;
    const uint64_t t = now_ns();
    std::lock_guard<std::mutex> lk(g_mu);
    for (const Seen& s : g_seen)
        if (t - s.ns <= kRecentNs && rptr(s.grp) == s.vtbl) out.push_back(s.grp);
    return out;
}

void read_name(uint64_t pane, char* out, size_t n) {
    out[0] = 0;
    if (!ok(pane + C().pane_name, 24)) return;
    size_t i = 0;
    for (; i + 1 < n && i < 24; ++i) {
        const char ch = rd<char>(pane + C().pane_name + i);
        if (!ch) break;
        out[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '?';
    }
    out[i] = 0;
}

// Registers the elements of the pointer-enabled groups on screen. Returns whether one has a button that takes input.
bool scan(mp::Engine* e, int* focused) {
    State& s = st();
    const Config& c = C();
    s.el_group.clear();
    s.n_groups = s.n_buttons = 0;
    bool any = false;
    for (uint64_t grp : recent_groups()) {
        if (!ok(grp, c.grp_flags + 4) || !(rd<uint32_t>(grp + c.grp_flags) & c.grp_pointer_bit)) continue;
        const uint64_t cur = rptr(grp + c.grp_current);
        ++s.n_groups;
        each_button(grp, [&](uint64_t btn) {
            if (!ok(btn, c.btn_hit + 8) || (rd<uint32_t>(btn + c.btn_flags) & c.btn_ready_mask) != c.btn_ready_mask) return;
            const uint64_t lay = rptr(btn + c.btn_layout), hit = rptr(btn + c.btn_hit);
            float lw, lh, cx, cy, hw, hh;
            if (!hit || !layout_size(lay, &lw, &lh) || !pane_visible(hit) || !hit_box(hit, &cx, &cy, &hw, &hh)) return;
            any = true;
            ++s.n_buttons;
            if (!e) return;
            const float sx = c.canvas_w / lw, sy = c.canvas_h / lh;
            mp::Element el;
            el.key = btn;
            el.x0 = c.canvas_w * 0.5f + (cx - hw) * sx;
            el.x1 = c.canvas_w * 0.5f + (cx + hw) * sx;
            el.y0 = c.canvas_h * 0.5f - (cy + hh) * sy;
            el.y1 = c.canvas_h * 0.5f - (cy - hh) * sy;
            el.flags = mp::kHover;
            el.layout = static_cast<uint32_t>(lay >> 4);   // no layout id in ui2d: the object's address (logs only)
            el.sub = static_cast<uint32_t>(grp >> 4);
            char pn[32], hn[32];
            read_name(rptr(hit + c.pane_parent), pn, sizeof pn);
            read_name(hit, hn, sizeof hn);
            std::snprintf(el.name, sizeof el.name, "%s/%s", pn, hn);
            const int idx = e->add(el);
            s.el_group.push_back(grp);
            if (btn == cur) *focused = idx;
        });
    }
    return any;
}

bool ready(void*) { return st().installed && st().enabled; }

void idle(void*) {
    State& s = st();
    g_feed.store(false, std::memory_order_relaxed);
    g_press_stage.store(0, std::memory_order_relaxed);
    g_press_grp.store(0, std::memory_order_relaxed);
    if (s.was_up && mp::engine().log_ok(true)) NXLOG_I(C().tag, "menu: closed");
    s.was_up = false;
    s.el_group.clear();
}

bool collect(mp::Engine& e, float, float, bool inside, void*) {
    State& s = st();
    int focused = -1;
    const bool any = scan(&e, &focused);
    e.set_selection(focused, any, any ? "(pointer group)" : "");
    // the mouse stands in for the touch while it is inside the picture and shown (with the pad in use the game's own input stays)
    const mm::Pointer& p = mm::pointer();
    g_held.store(p.held(), std::memory_order_relaxed);
    g_feed.store(inside && any, std::memory_order_relaxed);
    if (!s.was_up && mp::engine().log_ok(true))
        NXLOG_I(C().tag, "menu: open, %u pointer groups, %u buttons", s.n_groups, s.n_buttons);
    s.was_up = true;
    return any;
}

// Hover: the game focuses the button under the pointer itself (the fed position); the engine waits for the group's current button.
bool focus(const mp::Element&, void*) { return true; }

// Click: a trigger and then a release on the button, through the group's own pointer update.
bool confirm(const mp::Element& el, void*) {
    const State& s = st();
    const auto& els = mp::engine().elements();
    for (size_t i = 0; i < els.size() && i < s.el_group.size(); ++i) {
        if (els[i].key != el.key) continue;
        g_press_btn.store(el.key, std::memory_order_relaxed);
        g_press_stage.store(2, std::memory_order_relaxed);
        g_press_grp.store(s.el_group[i], std::memory_order_release);
        return true;
    }
    return false;
}

bool poll_scan(void*) {
    int focused = -1;
    return scan(nullptr, &focused);
}

mp::Adapter& adapter() {
    static mp::Adapter a;
    return a;
}

uint64_t key_u64(const char* sec, const char* key, uint64_t def) { return static_cast<uint64_t>(config_get_int(sec, key, static_cast<int64_t>(def))); }

}  // namespace

bool config_from_ini(const char* sec, Config* out) {
    Config c = *out;
    c.group_update = key_u64(sec, "ui2d_group_update", c.group_update);
    c.grp_buttons = key_u64(sec, "ui2d_grp_buttons", c.grp_buttons);
    c.grp_current = key_u64(sec, "ui2d_grp_current", c.grp_current);
    c.grp_flags = key_u64(sec, "ui2d_grp_flags", c.grp_flags);
    c.grp_pointer_bit = static_cast<uint32_t>(key_u64(sec, "ui2d_grp_pointer_bit", c.grp_pointer_bit));
    c.btn_node = key_u64(sec, "ui2d_btn_node", c.btn_node);
    c.btn_layout = key_u64(sec, "ui2d_btn_layout", c.btn_layout);
    c.btn_flags = key_u64(sec, "ui2d_btn_flags", c.btn_flags);
    c.btn_ready_mask = static_cast<uint32_t>(key_u64(sec, "ui2d_btn_ready_mask", c.btn_ready_mask));
    c.btn_hit = key_u64(sec, "ui2d_btn_hit", c.btn_hit);
    c.lay_size = key_u64(sec, "ui2d_lay_size", c.lay_size);
    c.pane_parent = key_u64(sec, "ui2d_pane_parent", c.pane_parent);
    c.pane_size = key_u64(sec, "ui2d_pane_size", c.pane_size);
    c.pane_flags = key_u64(sec, "ui2d_pane_flags", c.pane_flags);
    c.pane_alpha = key_u64(sec, "ui2d_pane_alpha", c.pane_alpha);
    c.pane_base = key_u64(sec, "ui2d_pane_base", c.pane_base);
    c.pane_mtx = key_u64(sec, "ui2d_pane_mtx", c.pane_mtx);
    c.pane_name = key_u64(sec, "ui2d_pane_name", c.pane_name);
    c.canvas_w = static_cast<float>(config_get_double(sec, "ui2d_canvas_w", c.canvas_w));
    c.canvas_h = static_cast<float>(config_get_double(sec, "ui2d_canvas_h", c.canvas_h));
    *out = c;
    return c.group_update != 0;
}

void install(uint64_t base, const Config& cfg, const char* section) {
    g_cfg = &cfg;
    State& s = st();
    s.base = base;
    s.section = section;
    s.enabled = config_get_bool(section, "menu_mouse", true);
    s.ig_enabled = config_get_bool(section, "menu_in_game", true);
    mp::engine().set_log_level(static_cast<int>(config_get_int(section, "menu_log", 1)));
    if (!cfg.group_update) {
        NXLOG_W(cfg.tag, "menu mouse (ui2d): [%s] ui2d_group_update is not set, off", section);
        return;
    }
    g_hook = hook_add_entry(base + cfg.group_update, &hook_group_update);
    mp::Adapter& a = adapter();
    a.tag = cfg.tag;
    a.canvas_w = cfg.canvas_w;
    a.canvas_h = cfg.canvas_h;
    a.ready = &ready;
    a.collect = &collect;
    a.idle = &idle;
    a.focus = &focus;
    a.confirm = &confirm;
    s.installed = true;
    NXLOG_I(cfg.tag, "menu mouse (ui2d) %s: button group pointer update at main+0x%llx; [%s] menu_log %d", s.enabled ? "on" : "off",
            static_cast<unsigned long long>(cfg.group_update), section, mp::engine().log_level());
}

void remove() {
    if (g_hook) hook_remove(g_hook);
    g_hook = 0;
    g_feed.store(false);
    st().installed = false;
    st().gate.reset();
    mp::engine().reset();
    std::lock_guard<std::mutex> lk(g_mu);
    g_seen.clear();
}

Act menu_frame(bool menu_up, const mm::Geometry& g) {
    // the pointer on the UI plane, for the hook
    float nx = 0, ny = 0;
    bool inside = false;
    mm::window_to_ui(g, mm::pointer().x(), mm::pointer().y(), &nx, &ny, &inside);
    g_nx.store(fbits(nx), std::memory_order_relaxed);
    g_ny.store(fbits(ny), std::memory_order_relaxed);
    return mp::engine().frame(menu_up, g, adapter());
}

bool menu_over_value() { return mp::engine().over_value(); }

bool menu_active() { return mp::engine().active(); }

// A pointer-enabled button group on screen while the game is in play: one of its menus (inferred: the game enables the pointer on menu
// groups only; docs/menu_pointer.md).
bool menu_in_game(bool in_game) {
    State& s = st();
    mp::Gate::Event ev = mp::Gate::kNone;
    const bool up = s.gate.poll(in_game && s.installed && s.enabled && s.ig_enabled, mp::engine(), &poll_scan, nullptr, &ev);
    if (ev == mp::Gate::kOpened && mp::engine().log_ok(true))
        NXLOG_I(C().tag, "menu: in-game menu up: %u pointer groups, %u buttons; the mouse is the menu pointer", s.n_groups, s.n_buttons);
    else if ((ev == mp::Gate::kClosed || ev == mp::Gate::kLeft) && mp::engine().log_ok(true))
        NXLOG_I(C().tag, "menu: in-game menu closed%s", ev == mp::Gate::kLeft ? " (the game left play)" : "");
    return up;
}

void status_text(char* buf, size_t n) {
    const State& s = st();
    const mp::Engine& e = mp::engine();
    std::snprintf(buf, n, " menu_groups=%u menu_buttons=%u hover='%s' selected='%s' clicks=%u group_updates=%llu fed=%llu presses=%llu in_game_menu=%d",
                  s.n_groups, s.n_buttons, e.hover_name(), e.focus_name(), e.clicks(), static_cast<unsigned long long>(g_hook_calls.load()),
                  static_cast<unsigned long long>(g_feeds.load()), static_cast<unsigned long long>(g_presses.load()), s.gate.up() ? 1 : 0);
}

}  // namespace nxrt::play::ui2d
