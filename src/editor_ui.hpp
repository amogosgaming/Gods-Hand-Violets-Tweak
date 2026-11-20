// God's Hand of Principality and Demise. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// The editors' widgets (chart, character, stage and event editors): an
// immediate-mode panel toolkit drawn in canvas units. Each frame the editor
// fills in the mouse, clicks and wheel, then calls the widgets, which draw
// themselves and say what changed. On touchscreens, fields use the native
// keyboard dialog (platform::askText), lists & panels drag-scroll, and tap
// targets are enlarged while preserving full desktop functionality.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common.hpp"
#include "gfx.hpp"
#include "input.hpp"
#include "platform.hpp"
#include "raylib.h"

namespace gh::edui {

using gfx::Align;
using gfx::FontId;

inline std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c >= 0x20) o += c;
    }
    return o + "\"";
}

inline std::string num(double v) {
    char b[40];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.1f", v);
    else std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    if (s.find('.') != std::string::npos && s.find('e') == std::string::npos) {
        while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    }
    return s;
}

inline std::string utf8(char32_t c) {
    std::string o;
    if (c < 0x80) o += char(c);
    else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 0x3F)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    return o;
}

inline void popUtf8(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = (unsigned char)s.back();
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

inline const Color kPanel{12, 8, 20, 232};
inline const Color kInk{232, 226, 246, 255};
inline const Color kDim{170, 160, 196, 255};
inline const Color kAccent{255, 214, 120, 255};

struct Ui {
    Vector2 mouse{};
    bool click = false, rclick = false, down = false, released = false;
    bool touchMode = false;
    float wheel = 0;
    const Input* in = nullptr;
    std::string focus;  // id of the text field being typed into
    bool used = false;  // the mouse/touch did something in the panel this frame

    // Touch tap-vs-drag tracking (so swiping to scroll never triggers a button).
    Vector2 pressPos{};
    bool trackingPress = false;
    bool dragged = false;
    bool tapReady = false;

    // Optional clip region for scrollable panels/lists.
    bool hasClip = false;
    Rectangle clipRect{};
    std::string scrollDragId;
    float scrollDragLastY = 0;

    // Android native keyboard dialog state (platform::askText).
    std::string askId;

    // An open drop-down list (drawn over everything by overlays()).
    std::string dropId, dropChanged;
    Rectangle dropRect{};
    std::vector<std::string> dropOpts;
    int dropCur = 0, dropPicked = 0;  // the item shown when it opened; the one clicked
    float dropScroll = 0;
    bool dropJustOpened = false;  // the click that opened it must not close it
    bool dropDragging = false;
    float dropDragY = 0;
    static constexpr int kDropRows = 12;

    void syncPointer(const Input& input, Vector2 canvasPos, bool touch, bool blocked = false) {
        in = &input;
        touchMode = touch;
        mouse = canvasPos;
        const bool pressed = input.mousePressed(MOUSE_BUTTON_LEFT) && !blocked;
        const bool isDown = IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !blocked;
        const bool rel = input.mouseReleased(MOUSE_BUTTON_LEFT);
        click = pressed;
        rclick = input.mousePressed(MOUSE_BUTTON_RIGHT);
        down = isDown;
        released = rel;
        wheel = input.wheel();
        used = false;
        tapReady = false;
        if (pressed) {
            pressPos = canvasPos;
            trackingPress = true;
            dragged = false;
        }
        if (trackingPress && isDown) {
            if (std::hypot(canvasPos.x - pressPos.x, canvasPos.y - pressPos.y) > 10.0f) dragged = true;
        }
        if (rel) {
            if (trackingPress && !dragged && !blocked) tapReady = true;
            trackingPress = false;
            scrollDragId.clear();
            dropDragging = false;
        }
        if (!isDown && !rel) {
            trackingPress = false;
            scrollDragId.clear();
            dropDragging = false;
        }
    }

    // Desktop activates on press; touch activates on release when not dragged.
    bool tap() const { return touchMode ? tapReady : click; }

    // Widgets under an open list or outside an active scroll clip do not see the mouse.
    bool hover(Rectangle r) const {
        if (!CheckCollisionPointRec(mouse, r)) return false;
        if (hasClip && !CheckCollisionPointRec(mouse, clipRect)) return false;
        if (!dropId.empty() && CheckCollisionPointRec(mouse, dropRect)) return false;
        return true;
    }

    void box(Rectangle r, bool on = false, bool hot = false) const {
        DrawRectangleRounded(r, 0.25f, 6, on ? Color{90, 66, 140, 255} : hot ? Color{58, 44, 88, 255} : Color{36, 28, 56, 255});
        DrawRectangleRoundedLinesEx(r, 0.25f, 6, 1.5f, on ? kAccent : Color{110, 92, 150, 200});
    }

    bool button(Rectangle r, const std::string& label, bool on = false) {
        const bool h = hover(r);
        const bool active = h && (touchMode ? down : true);
        box(r, on, active);
        const float fs = touchMode && r.height >= 34.0f ? 19.0f : 18.0f;
        gfx::text(FontId::Ui, label, r.x + r.width * 0.5f, r.y + (r.height - fs) * 0.5f, fs, kInk, Align::Center);
        if (h && tap()) {
            used = true;
            focus.clear();
            return true;
        }
        return false;
    }

    bool check(Rectangle r, const std::string& label, bool& v) {
        const float bs = touchMode ? 22.0f : 18.0f;
        const Rectangle b{r.x, r.y + (r.height - bs) * 0.5f, bs, bs};
        const bool h = hover(r);
        box(b, v, h);
        if (v) gfx::text(FontId::Ui, "x", b.x + bs * 0.5f, b.y + (bs - 18.0f) * 0.5f - 1.0f, 18, kAccent, Align::Center);
        gfx::text(FontId::Ui, label, r.x + bs + 10.0f, r.y + (r.height - 18) * 0.5f, 18, kInk);
        if (h && tap()) {
            v = !v;
            used = true;
            focus.clear();
            return true;
        }
        return false;
    }

    // Begin/end a clipped vertical scroll viewport (wheel on desktop, finger drag on touch).
    void beginScroll(const std::string& id, Rectangle view, float& scroll) {
        const bool over = CheckCollisionPointRec(mouse, view) && dropId.empty();
        if (over && wheel != 0) {
            scroll = std::max(0.0f, scroll - wheel * 36.0f);
            used = true;
        }
        if (touchMode && over && click) {
            scrollDragId = id;
            scrollDragLastY = mouse.y;
            used = true;
        }
        if (scrollDragId == id && down) {
            const float dy = mouse.y - scrollDragLastY;
            scrollDragLastY = mouse.y;
            if (dragged && std::fabs(dy) > 0.01f) {
                scroll = std::max(0.0f, scroll - dy);
                used = true;
            }
        }
        hasClip = true;
        clipRect = view;
        const Vector2 o = gfx::canvasOrigin();
        const float cs = gfx::canvasScale();
        BeginScissorMode(int(o.x + view.x * cs), int(o.y + view.y * cs), std::max(1, int(view.width * cs)),
                         std::max(1, int(view.height * cs)));
    }

    void endScroll(Rectangle view, float& scroll, float contentH) {
        EndScissorMode();
        hasClip = false;
        const float maxS = std::max(0.0f, contentH - view.height);
        scroll = clampv(scroll, 0.0f, maxS);
        if (maxS > 1.0f) {
            const float barH = std::max(26.0f, view.height * (view.height / contentH));
            const float barY = view.y + (view.height - barH) * (scroll / maxS);
            DrawRectangleRounded({view.x + view.width - 5.0f, barY, 4.0f, barH}, 0.5f, 4, Color{180, 165, 220, 165});
        }
    }

    // label  [-] value [+]   (the wheel over it works too; tap opens native keyboard on Android)
    bool stepper(Rectangle r, const std::string& label, double& v, double step, double lo, double hi, const char* fmt) {
        gfx::text(FontId::Ui, label, r.x, r.y + (r.height - 18) * 0.5f, 18, kDim);
        const float ctrlW = touchMode ? std::min(176.0f, r.width) : std::min(150.0f, r.width);
        const float bw = touchMode ? 36.0f : 28.0f;
        const float vx = r.x + r.width - ctrlW;
        const Rectangle minus{vx, r.y, bw, r.height}, plus{r.x + r.width - bw, r.y, bw, r.height};
        bool changed = false;
        const bool shift = in && (in->isDown(KEY_LEFT_SHIFT) || in->isDown(KEY_RIGHT_SHIFT));
        const double s = shift ? step * 10 : step;
        if (button(minus, "-")) { v -= s; changed = true; }
        if (button(plus, "+")) { v += s; changed = true; }
        const Rectangle mid{vx + bw + 2, r.y, std::max(20.0f, ctrlW - 2 * bw - 4), r.height};
        const std::string id = "num:" + label + "@" + std::to_string(int(r.x)) + "," + std::to_string(int(r.y));

        if (askId == id) {
            std::string res;
            const platform::TextAsk st = platform::textStatus(&res);
            if (st == platform::TextAsk::Done) {
                numText = res;
                changed = commitNumber(v) || changed;
                platform::resetText();
                askId.clear();
                focus.clear();
                numId.clear();
            } else if (st == platform::TextAsk::Cancelled) {
                platform::resetText();
                askId.clear();
                focus.clear();
                numId.clear();
            }
        }
        if (numId == id && focus != id && askId != id) {  // left by a click elsewhere: keep what was typed
            changed = commitNumber(v) || changed;
            numId.clear();
        }
        if (hover(mid) && tap()) {
            used = true;
            if (touchMode && platform::askText(label.empty() ? "Value" : label, TextFormat(fmt, v))) {
                askId = id;
                focus = id;
                numId = id;
                numText.clear();
            } else {
                focus = numId = id;
                numText.clear();
            }
        }
        const bool typing = focus == id && askId != id;
        if (typing && in) {
            for (char32_t c : in->typed())
                if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+') numText += char(c);
            if (in->pressed(KEY_BACKSPACE) && !numText.empty()) numText.pop_back();
            if (in->pressed(KEY_ESCAPE)) {
                focus.clear();
                numId.clear();
            } else if (in->pressed(KEY_ENTER) || in->pressed(KEY_KP_ENTER) || in->pressed(KEY_TAB)) {
                changed = commitNumber(v) || changed;
                focus.clear();
                numId.clear();
            }
        }
        if (hover(mid) && wheel != 0 && !typing) {
            v += (wheel > 0 ? s : -s);
            changed = true;
            used = true;
        }
        v = clampv(v, lo, hi);
        box(mid, focus == id);
        const std::string shown = typing ? numText + (std::fmod(GetTime(), 1.0) < 0.5 ? "|" : "") : std::string(TextFormat(fmt, v));
        gfx::text(FontId::Ui, shown, mid.x + mid.width * 0.5f, r.y + (r.height - 18) * 0.5f, 18, kInk, Align::Center);
        return changed;
    }
    std::string numId, numText;  // the number box being typed into, and what was typed
    bool commitNumber(double& v) {
        char* end = nullptr;
        const double d = std::strtod(numText.c_str(), &end);
        if (numText.empty() || end == numText.c_str() || !std::isfinite(d)) return false;
        v = d;
        return true;
    }

    // A text field: click to type, ENTER or a click elsewhere to finish.
    // On Android/touch, opens the native text input dialog via platform::askText().
    bool text(const std::string& id, Rectangle r, const std::string& label, std::string& s) {
        float fx = r.x;
        if (!label.empty()) {
            gfx::text(FontId::Ui, label, r.x, r.y + (r.height - 18) * 0.5f, 18, kDim);
            fx = r.x + 92;
        }
        const Rectangle f{fx, r.y, r.x + r.width - fx, r.height};
        const bool h = hover(f);
        bool changed = false;
        if (askId == id) {
            std::string res;
            const platform::TextAsk st = platform::textStatus(&res);
            if (st == platform::TextAsk::Done) {
                if (res != s) {
                    s = res;
                    changed = true;
                }
                platform::resetText();
                askId.clear();
                focus.clear();
            } else if (st == platform::TextAsk::Cancelled) {
                platform::resetText();
                askId.clear();
                focus.clear();
            }
        }
        if (h && tap()) {
            used = true;
            if (touchMode && platform::askText(label.empty() ? id : label, s)) {
                askId = id;
                focus = id;
            } else {
                focus = id;
            }
        }
        const bool on = focus == id;
        box(f, on, h);
        if (on && askId != id && in) {
            for (char32_t c : in->typed())
                if (c >= 32 && c != 127) {
                    s += utf8(c);
                    changed = true;
                }
            if (in->pressed(KEY_BACKSPACE) && !s.empty()) {
                popUtf8(s);
                changed = true;
            }
            if (in->pressed(KEY_ENTER) || in->pressed(KEY_KP_ENTER) || in->pressed(KEY_TAB)) focus.clear();
        }
        // Intersect scissor with any active outer scroll clip so scrolled text never bleeds.
        Rectangle sc{f.x + 4, f.y, std::max(0.0f, f.width - 8), f.height};
        if (hasClip) {
            const float x0 = std::max(sc.x, clipRect.x), y0 = std::max(sc.y, clipRect.y);
            const float x1 = std::min(sc.x + sc.width, clipRect.x + clipRect.width);
            const float y1 = std::min(sc.y + sc.height, clipRect.y + clipRect.height);
            sc = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
        }
        if (sc.width > 0 && sc.height > 0) {
            const Vector2 o = gfx::canvasOrigin();
            const float cs = gfx::canvasScale();
            BeginScissorMode(int(o.x + sc.x * cs), int(o.y + sc.y * cs), int(sc.width * cs), int(sc.height * cs));
            const std::string shown = s + (on && askId != id && std::fmod(GetTime(), 1.0) < 0.5 ? "|" : "");
            const float tw = gfx::measure(FontId::Ui, shown, 18).x;
            gfx::text(FontId::Ui, shown, std::min(f.x + 6, f.x + f.width - 8 - tw), f.y + (f.height - 18) * 0.5f, 18, kInk);
            if (hasClip) {
                BeginScissorMode(int(o.x + clipRect.x * cs), int(o.y + clipRect.y * cs), std::max(1, int(clipRect.width * cs)),
                                 std::max(1, int(clipRect.height * cs)));
            } else {
                EndScissorMode();
            }
        }
        return changed;
    }

    // label  [option  v]   A click opens the list (wheel or touch-drag to scroll it);
    // a click/tap on an item picks it. True on the frame after a new pick.
    bool dropdown(const std::string& id, Rectangle r, const std::string& label, const std::vector<std::string>& opts, int& idx,
                  float labelW = 92) {
        if (opts.empty()) return false;
        bool changed = false;
        if (dropChanged == id) {  // picked in the list last frame
            dropChanged.clear();
            idx = dropPicked;
            changed = true;
        }
        gfx::text(FontId::Ui, label, r.x, r.y + (r.height - 18) * 0.5f, 18, kDim);
        const Rectangle f{r.x + labelW, r.y, r.width - labelW, r.height};
        idx = clampv(idx, 0, int(opts.size()) - 1);
        const bool open = dropId == id;
        const bool h = hover(f);
        box(f, open, h);
        const std::string& o = opts[size_t(idx)];
        Rectangle sc{f.x + 4, f.y, std::max(0.0f, f.width - 30), f.height};
        if (hasClip) {
            const float x0 = std::max(sc.x, clipRect.x), y0 = std::max(sc.y, clipRect.y);
            const float x1 = std::min(sc.x + sc.width, clipRect.x + clipRect.width);
            const float y1 = std::min(sc.y + sc.height, clipRect.y + clipRect.height);
            sc = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
        }
        if (sc.width > 0 && sc.height > 0) {
            const Vector2 org = gfx::canvasOrigin();
            const float cs = gfx::canvasScale();
            BeginScissorMode(int(org.x + sc.x * cs), int(org.y + sc.y * cs), int(sc.width * cs), int(sc.height * cs));
            gfx::text(FontId::Ui, o.empty() ? "(normal)" : o, f.x + 8, f.y + (f.height - 18) * 0.5f, 18, kInk);
            if (hasClip) {
                BeginScissorMode(int(org.x + clipRect.x * cs), int(org.y + clipRect.y * cs), std::max(1, int(clipRect.width * cs)),
                                 std::max(1, int(clipRect.height * cs)));
            } else {
                EndScissorMode();
            }
        }
        gfx::text(FontId::Ui, open ? "^" : "v", f.x + f.width - 14, f.y + (f.height - 18) * 0.5f, 18, kAccent, Align::Center);
        if (h && tap()) {
            used = true;
            focus.clear();
            if (open) {
                dropId.clear();
            } else {
                dropId = id;
                dropOpts = opts;
                dropCur = idx;
                dropJustOpened = true;
                const int maxRows = touchMode ? 10 : kDropRows;
                const int rows = std::min(maxRows, int(opts.size()));
                const float rh = touchMode ? 34.0f : 24.0f;
                const float listH = rows * rh + 6;
                const float below = f.y + f.height + 2;
                dropRect = {f.x, below + listH <= gfx::H - 8 ? below : std::max(8.0f, f.y - 2 - listH), f.width, listH};
                dropScroll = float(std::clamp(idx - rows / 2, 0, int(opts.size()) - rows));
            }
        }
        return changed;
    }

    // Draws the open list (after the rest of the panel) and handles its clicks/drags.
    void overlays() {
        if (dropId.empty()) return;
        const int maxRows = touchMode ? 10 : kDropRows;
        const float rh = touchMode ? 34.0f : 24.0f;
        const int n = int(dropOpts.size()), rows = std::min(maxRows, n);
        const bool over = CheckCollisionPointRec(mouse, dropRect);
        if (over && wheel != 0) dropScroll = std::clamp(dropScroll - wheel * 3, 0.0f, float(std::max(0, n - rows)));
        if (touchMode && over && click) {
            dropDragging = true;
            dropDragY = mouse.y;
            used = true;
        }
        if (dropDragging && down) {
            const float dy = mouse.y - dropDragY;
            dropDragY = mouse.y;
            if (dragged && std::fabs(dy) > 0.01f) {
                dropScroll = std::clamp(dropScroll - dy / rh, 0.0f, float(std::max(0, n - rows)));
                used = true;
            }
        }
        DrawRectangleRec({dropRect.x - 2, dropRect.y - 2, dropRect.width + 4, dropRect.height + 4}, Color{6, 4, 12, 250});
        DrawRectangleLinesEx(dropRect, 1.5f, kAccent);
        const int first = std::clamp(int(dropScroll), 0, std::max(0, n - rows));
        for (int k = 0; k < rows && first + k < n; ++k) {
            const int i = first + k;
            const Rectangle it{dropRect.x + 3, dropRect.y + 3 + k * rh, dropRect.width - 6, rh};
            const bool h = CheckCollisionPointRec(mouse, it);
            if (i == dropCur || h) DrawRectangleRec(it, i == dropCur ? Color{90, 66, 140, 255} : Color{58, 44, 88, 255});
            gfx::text(FontId::Ui, dropOpts[size_t(i)].empty() ? "(normal)" : dropOpts[size_t(i)], it.x + 8, it.y + (rh - 18.0f) * 0.5f, 18, kInk);
            if (h && tap() && !dropJustOpened) {
                dropPicked = i;
                dropChanged = dropId;
                dropId.clear();
                used = true;
                return;
            }
        }
        if (n > rows) {  // where the list is
            const float bh = std::max(20.0f, dropRect.height * rows / n);
            const float by = dropRect.y + (dropRect.height - bh) * (dropScroll / float(std::max(1, n - rows)));
            DrawRectangleRec({dropRect.x + dropRect.width - 5, by, 4, bh}, kDim);
        }
        if (over && (click || tap())) used = true;
        if ((tap() && !over && !dropJustOpened) || (in && in->pressed(KEY_ESCAPE))) dropId.clear();
        dropJustOpened = false;
    }

    // label  < option >   (click/tap the arrows, or the wheel over it)
    bool choice(Rectangle r, const std::string& label, const std::vector<std::string>& opts, int& idx, float labelW = 92) {
        if (opts.empty()) return false;
        gfx::text(FontId::Ui, label, r.x, r.y + (r.height - 18) * 0.5f, 18, kDim);
        const float x0 = r.x + labelW, bw = touchMode ? 36.0f : 28.0f;
        const Rectangle prev{x0, r.y, bw, r.height}, next{r.x + r.width - bw, r.y, bw, r.height};
        const Rectangle mid{x0 + bw + 2, r.y, std::max(20.0f, r.width - labelW - 2 * bw - 4), r.height};
        const int n = int(opts.size());
        int d = 0;
        if (button(prev, "<")) d = -1;
        if (button(next, ">")) d = 1;
        if (hover(mid) && wheel != 0) {
            d = wheel > 0 ? -1 : 1;
            used = true;
        }
        if (d) idx = ((idx + d) % n + n) % n;
        idx = clampv(idx, 0, n - 1);
        box(mid);
        const std::string& o = opts[size_t(idx)];
        gfx::text(FontId::Ui, o.empty() ? "(normal)" : o, mid.x + mid.width * 0.5f, r.y + (r.height - 18) * 0.5f, 18, kInk,
                  Align::Center);
        return d != 0;
    }
};

}  // namespace gh::edui
