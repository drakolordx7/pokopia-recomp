// SPDX-License-Identifier: GPL-3.0-or-later
// The menu pointer engine: hover, click and the menu gate over a game's on-screen UI elements, for any UI library (docs/menu_pointer.md).
//
// An engine adapter (menu_ktgl.h, menu_ui2d.h) knows one UI library: once a frame it registers the elements on screen (a rectangle on its
// canvas, an identity that stays the same across frames, a few kind bits) and says which one the game has selected. The engine does the
// rest, the same way for every library:
//   * hit-test: the element under the pointer is the smallest hoverable one containing it (the first registered on a tie);
//   * hover -> focus: when the pointer moves onto another element the game's selection is moved there, through the adapter's focus call
//     or, without one, by d-pad presses toward it (one press, then a wait for the selection to move; the other axis when a press moved
//     nothing; given up after 4 failed presses or 40 presses);
//   * click -> confirm: a click on the selected element confirms it (the adapter's confirm call, or A); on another element it is selected
//     first and confirmed once the selection has held still for 2 frames; an element with its own button (a key guide) sends that button;
//     with no selection marker on screen (messages, prompts) a click anywhere is A;
//   * wheel -> value: the pointer over a slider element makes the wheel step its value (play.cpp asks over_value());
//   * the gate: menus the game opens while in play (it keeps calling its input function under them) are found by polling the adapter
//     (Gate below), so play mode can treat those frames as menu frames.
// Elements live for one frame: the adapter registers them again every frame. No game or library is named here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "menu_mouse.h"
#include "play_types.h"

namespace nxrt::play::mp {

enum : uint32_t {
    kHover = 1u << 0,      // the pointer can hover it (and so select and confirm it)
    kSlider = 1u << 1,     // a slider track: the wheel steps its value while the pointer is over it
};

struct Element {
    uint64_t key = 0;                 // the game object behind it: the same across frames
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // canvas units, y down
    uint32_t flags = 0;               // kHover, kSlider
    Act click = Act::None;            // a click on it sends this button (a key guide); checked before everything else
    uint32_t layout = 0, sub = 0;     // the layout it belongs to (logs)
    char name[48] = {};
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
    float area() const { return (x1 - x0) * (y1 - y0); }
    bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

class Engine;

// One UI library's side. Calls come on the main thread from Engine::frame.
struct Adapter {
    const char* tag = "menu";             // log tag
    float canvas_w = 1920.f, canvas_h = 1080.f;   // the canvas the elements are in; the pointer's UI plane (-1..1, centred 16:9) maps onto it
    void* user = nullptr;
    bool (*ready)(void* user) = nullptr;  // installed and switched on (nullptr: always)
    // The frame's elements: register them with Engine::add, then Engine::set_selection. px, py: the pointer on the canvas; `inside`: it
    // is inside the picture and the cursor is shown (hit-tests apply). Returns whether a menu is on screen (the gate reuses it).
    bool (*collect)(Engine& e, float px, float py, bool inside, void* user) = nullptr;
    void (*idle)(void* user) = nullptr;   // the pointer is not active this frame
    // Selects the element directly. False (or nullptr): the engine steps the d-pad toward it.
    bool (*focus)(const Element& el, void* user) = nullptr;
    // Confirms the element directly. False (or nullptr): the engine sends A.
    bool (*confirm)(const Element& el, void* user) = nullptr;
};

class Engine {
public:
    // Once per rendered frame on the main thread (play_game.h GamePlay::menu_frame): the button to pulse, or Act::None.
    Act frame(bool menu_up, const mm::Geometry& g, const Adapter& a);

    // ---- for the adapter's collect ----
    int add(const Element& el);                       // the element's index this frame
    // The game's selection this frame (an index from add, -1 = none) and whether its selection marker (a cursor) is on screen.
    // marker_name: the marker's name for the log.
    void set_selection(int focused, bool marker, const char* marker_name = "");
    const std::vector<Element>& elements() const { return els_; }

    // ---- what the frame found ----
    bool active() const { return active_; }          // the pointer was active this frame
    bool over_value() const { return active_ && slider_ >= 0; }
    bool marker() const { return active_ && marker_; }
    int hovered() const { return hovered_; }
    int focused() const { return focused_; }
    // collect's result of the last frame, once (the gate reuses a scan of this frame instead of scanning again)
    bool take_frame_result(bool* present);

    // ---- logging (shared by the adapter, so both obey one rate limit) ----
    void set_log_level(int n) { log_level_ = n; }
    int log_level() const { return log_level_; }
    bool log_ok(bool force = false);
    void add_log_lines(uint32_t n) { log_lines_ += n; }
    uint32_t log_lines() const { return log_lines_; }

    // ---- status ----
    const char* hover_name() const { return hover_name_; }
    const char* focus_name() const { return focus_name_; }
    uint32_t steps() const { return n_steps_; }
    uint32_t clicks() const { return n_clicks_; }

    void reset();                                    // forget the navigation (remove)

private:
    void clear_nav();
    int find(uint64_t key) const;
    Act confirm(const Adapter& a, const Element& el);

    std::vector<Element> els_;
    int hovered_ = -1, focused_ = -1, slider_ = -1;
    bool marker_ = false;
    bool active_ = false;
    bool frame_scanned_ = false, frame_present_ = false;
    // navigation toward the element under the pointer
    uint64_t target_ = 0;
    int steps_ = 0, wait_ = 0, fails_ = 0, click_wait_ = 0, focus_stable_ = 0;
    bool pending_confirm_ = false, prefer_other_axis_ = false;
    uint64_t focus_before_ = 0, last_focus_ = 0, last_hover_ = 0;
    // logging and status
    int log_level_ = 1;
    uint32_t log_lines_ = 0;
    uint64_t last_log_ns_ = 0;
    uint64_t idle_scan_ns_ = 0;                          // the last scan while the mouse was not in use (frame())
    static constexpr uint64_t kIdleScanNs = 250000000;   // 250 ms
    char hover_name_[48] = {}, focus_name_[48] = {}, marker_name_[48] = {};
    uint32_t n_steps_ = 0, n_clicks_ = 0;
};

Engine& engine();                                     // the one engine of the process

// Menus over play: the game is in play (it calls its input function) while one of its menus is up. Closed: one poll every kPollFrames
// frames; up: the menu frame's own scan of the frame is reused (Engine::take_frame_result). Up after kOpenHits positive polls in a row,
// down after kCloseFrames negative frames.
class Gate {
public:
    static constexpr int kPollFrames = 6, kOpenHits = 2, kCloseFrames = 10;
    enum Event { kNone, kOpened, kClosed, kLeft };   // kLeft: closed because the game left play (or the feature is off)
    // live: in play and the feature on. scan: the adapter's own poll (scans the screen, true = a menu is up).
    bool poll(bool live, Engine& e, bool (*scan)(void* user), void* user, Event* ev);
    bool up() const { return up_; }
    uint32_t opens() const { return opens_; }
    void reset() { up_ = false; hits_ = miss_ = tick_ = 0; }
private:
    bool up_ = false;
    int tick_ = 0, hits_ = 0, miss_ = 0;
    uint32_t opens_ = 0;
};

// The d-pad press that moves a selection at `from` toward `to` (the axis with the larger distance; the other one when other_axis and
// both distances are over 4 units).
Act dir_toward(const Element& from, const Element& to, bool other_axis);

}  // namespace nxrt::play::mp
