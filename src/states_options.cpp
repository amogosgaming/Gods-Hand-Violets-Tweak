// Astralis Engine. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// Psych Engine-style Options menu with subsections, key bindings, note colors,
// note skins, and audio offset calibration.
#include <array>
#include <cmath>
#include <functional>

#include "app.hpp"
#include "chart_binary.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "note_raster.hpp"
#include "platform.hpp"
#include "psych_ui.hpp"
#include "raylib.h"
#include "synth.hpp"

namespace gh {

using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {
const Color kGold{255, 222, 64, 255};   // the accent: a bright, cartoon yellow
const Color kIvory{242, 244, 255, 255};  // text: a cool white

struct Row {
    enum Kind { Bool, Number, Choice, Action, Key, AsmCpu } kind;
    std::string label, help;
    bool* b = nullptr;
    float* f = nullptr;
    float mn = 0, mx = 1, step = 0.1f;
    int* i = nullptr;
    std::vector<int> choices;
    std::vector<std::string> names;  // shown for each choice (default: frames per second)
    std::function<std::string(float)> fmt;
    std::function<void()> action;
    std::function<std::string()> value;  // Action rows: shown before the arrow
    int lane = 0;
};

struct OptionCategory {
    std::string title;
    std::string subtitle;
    std::function<void()> directAction;  // non-empty: opens a dedicated screen (Note Colors, etc.)
    std::vector<Row> rows;
};

// Remember which subsection and row Options was on when returning from a sub-screen.
int gLastCategory = 0;
int gLastOpenSection = -1;  // -1 = hub
bool gLastInDelayCombo = false;
std::string gLastOptionsRow;

// Word-wrap using Psych VCR font width when available, else FontId::Ui.
std::vector<std::string> wrapWidth(const PsychUi& ui, bool usePsych, FontId f, const std::string& s, float size, float width) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    auto meas = [&](const std::string& t) {
        return usePsych ? ui.textWidth(t, size) : gfx::measure(f, t, size).x;
    };
    while (i < s.size()) {
        const size_t sp = s.find(' ', i);
        const std::string word = s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        i = sp == std::string::npos ? s.size() : sp + 1;
        if (word.empty()) continue;
        const std::string longer = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && meas(longer) > width) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = longer;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}
}  // namespace

class OptionsState : public State {
public:
    void enter() override {
        buildCategories();
        catSel_ = clampv(gLastCategory, 0, int(cats_.size()) - 1);
        catAnim_ = float(catSel_);
        openSection_ = (gLastOpenSection >= 0 && gLastOpenSection < int(cats_.size()) && !cats_[size_t(gLastOpenSection)].rows.empty())
                           ? gLastOpenSection
                           : -1;
        inDelayCombo_ = openSection_ >= 0 && gLastInDelayCombo;
        sel_ = 0;
        const auto& rows = activeRows();
        if (openSection_ >= 0 && !rows.empty()) {
            for (int i = 0; i < int(rows.size()); ++i)
                if (rows[size_t(i)].label == gLastOptionsRow) sel_ = i;
        }
        anim_ = float(sel_);
        lastToggleRow_ = -1;
        lastToggleAt_ = -10.0;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        if (askingStorage_) {
            std::string path;
            const platform::TextAsk st = platform::textStatus(&path);
            if (st == platform::TextAsk::Done || st == platform::TextAsk::Cancelled) {
                askingStorage_ = false;
                platform::resetText();
                if (st == platform::TextAsk::Done && !path.empty()) applyStoragePath(path);
            }
            return;
        }
        if (openSection_ < 0) {
            updateHub(dt);
            return;
        }
        auto& rows = activeRows();
        if (rows.empty()) {
            inDelayCombo_ = false;
            gLastInDelayCombo = false;
            openSection_ = -1;
            gLastOpenSection = -1;
            return;
        }
        sel_ = clampv(sel_, 0, int(rows.size()) - 1);
        anim_ = approach(anim_, float(sel_), 14.0f, float(dt));
        Row& r = rows[size_t(sel_)];

        if (waiting_) {
            const int code = in.firstPressed();
            if (code == KEY_ESCAPE) { waiting_ = false; sfxPlay(Sfx::Cancel); }
            else if (code != 0) {
                a.settings.keys4[r.lane][slot_] = code == KEY_BACKSPACE ? 0 : code;
                waiting_ = false;
                sfxPlay(Sfx::Confirm);
                a.saveSettings();
            }
            return;
        }

        if (in.up()) {
            sel_ = (sel_ + int(rows.size()) - 1) % int(rows.size());
            gLastOptionsRow = rows[size_t(sel_)].label;
            sfxPlay(Sfx::Scroll);
        }
        if (in.down()) {
            sel_ = (sel_ + 1) % int(rows.size());
            gLastOptionsRow = rows[size_t(sel_)].label;
            sfxPlay(Sfx::Scroll);
        }

        const int dir = in.left() ? -1 : (in.right() ? 1 : 0);
        const bool shift = in.isDown(KEY_LEFT_SHIFT) || in.isDown(KEY_RIGHT_SHIFT);
        bool changed = false;
        switch (r.kind) {
            case Row::Bool:
                if (dir || in.accept()) {
                    if (r.label == "Fullscreen") a.toggleFullscreen();
                    else *r.b = !*r.b;
                    if (r.label == "VSync") platform::setVsync(a.settings.vsync);
                    if (r.b == &a.settings.antialiasing) psychUi().setSmooth(a.settings.antialiasing);
                    lastToggleRow_ = sel_;
                    lastToggleAt_ = a.now;
                    changed = true;
                }
                break;
            case Row::Number:
                if (dir) {
                    *r.f = clampv(*r.f + dir * r.step * (shift ? 10.0f : 1.0f), r.mn, r.mx);
                    if (r.step < 1) *r.f = std::round(*r.f / r.step) * r.step;
                    changed = true;
                }
                break;
            case Row::Choice:
                if (dir) {
                    size_t idx = 0;
                    for (size_t k = 0; k < r.choices.size(); ++k)
                        if (r.choices[k] == *r.i) idx = k;
                    idx = (idx + r.choices.size() + size_t(dir)) % r.choices.size();
                    *r.i = r.choices[idx];
                    changed = true;
                }
                break;
            case Row::AsmCpu: {
                // 3 CPU options: 0 = x86 SSE2 (enabled), 1 = x86-64 SSE4 (disabled), 2 = AVX2 (disabled).
                if (dir || in.accept()) {
                    const int cur = clampv(a.settings.noteAsmCpu, 0, 2);
                    const int next = (cur + (dir ? dir : 1) + 3) % 3;
                    const auto opt = static_cast<raster::CpuAsmOption>(next);
                    if (raster::cpuAsmEnabled(opt)) {
                        a.settings.noteAsmCpu = next;
                        changed = true;
                    } else {
                        a.settings.noteAsmCpu = 0;
                        sfxPlay(Sfx::Cancel);
                        a.notify(std::string(raster::cpuAsmLabel(opt)) + " ASM option is currently disabled (using x86 SSE2)", 3.5);
                    }
                }
                break;
            }
            case Row::Action:
                if (in.accept()) {
                    gLastCategory = openSection_;
                    gLastOpenSection = openSection_;
                    gLastInDelayCombo = inDelayCombo_;
                    gLastOptionsRow = r.label;
                    sfxPlay(Sfx::Confirm);
                    r.action();
                    return;
                }
                break;
            case Row::Key:
                if (dir) { slot_ = 1 - slot_; sfxPlay(Sfx::Scroll); }
                if (in.pressed(KEY_BACKSPACE)) { a.settings.keys4[r.lane][slot_] = 0; a.saveSettings(); }
                else if (in.accept()) { waiting_ = true; sfxPlay(Sfx::Scroll); }
                break;
        }
        if (changed) {
            sfxPlay(Sfx::Scroll);
            a.applyVolumes();
            a.saveSettings();
        }
        if (in.back() && !in.pressed(KEY_BACKSPACE)) {
            a.saveSettings();
            sfxPlay(Sfx::Cancel);
            if (inDelayCombo_) {
                inDelayCombo_ = false;
                gLastInDelayCombo = false;
                sel_ = 0;
                const auto& parentRows = cats_[size_t(openSection_)].rows;
                for (int i = 0; i < int(parentRows.size()); ++i)
                    if (parentRows[size_t(i)].label == "Adjust Delay and Combo") sel_ = i;
                anim_ = float(sel_);
                lastToggleRow_ = -1;
            } else {
                openSection_ = -1;
                gLastOpenSection = -1;
                lastToggleRow_ = -1;
            }
        }
    }

    void draw() override {
        if (openSection_ < 0) drawHub();
        else drawSection();
    }

    bool keyReserved(int) const override { return waiting_ || askingStorage_; }

private:
    std::vector<Row>& activeRows() {
        return inDelayCombo_ ? delayComboRows_ : cats_[size_t(openSection_)].rows;
    }
    const std::vector<Row>& activeRows() const {
        return inDelayCombo_ ? delayComboRows_ : cats_[size_t(openSection_)].rows;
    }

    void applyStoragePath(const std::string& path) {
        App& a = app();
        if (!platform::setStorageDir(path)) {
            a.notify("Could not update storage path", 3.0);
            return;
        }
        a.base = platform::dataDir();
        platform::prepareDataDir(a.base);
        a.userDir = a.base;
        setChartCacheDir(a.userDir + "cache/charts/");
        a.saveSettings();
        a.scores.load(a.userDir + "scores.cfg");
        a.library.scan(a.base);
        a.applyPsychUi();
        sfxPlay(Sfx::Confirm);
        a.notify("Storage location: " + platform::displayPath(a.base), 4.0);
    }

    void updateHub(double dt) {
        App& a = app();
        Input& in = a.input;
        const int nc = int(cats_.size());
        catAnim_ = approach(catAnim_, float(catSel_), 14.0f, float(dt));
        if (in.up()) { catSel_ = (catSel_ + nc - 1) % nc; gLastCategory = catSel_; sfxPlay(Sfx::Scroll); }
        if (in.down()) { catSel_ = (catSel_ + 1) % nc; gLastCategory = catSel_; sfxPlay(Sfx::Scroll); }
        if (in.accept()) {
            const OptionCategory& c = cats_[size_t(catSel_)];
            gLastCategory = catSel_;
            gLastInDelayCombo = false;
            sfxPlay(Sfx::Confirm);
            if (c.directAction) {
                gLastOpenSection = -1;
                c.directAction();
            } else {
                openSection_ = catSel_;
                inDelayCombo_ = false;
                gLastOpenSection = openSection_;
                sel_ = 0;
                anim_ = 0;
                lastToggleRow_ = -1;
                if (!c.rows.empty()) gLastOptionsRow = c.rows[0].label;
            }
            return;
        }
        if (in.back() && !in.pressed(KEY_BACKSPACE)) {
            a.saveSettings();
            gLastOpenSection = -1;
            gLastInDelayCombo = false;
            sfxPlay(Sfx::Cancel);
            if (a.optionsReturn) {
                const PlayRequest req = *a.optionsReturn;
                a.optionsReturn.reset();
                a.change(makePlay(req));
            } else {
                a.change(makeMainMenu());
            }
        }
    }

    // Psych Engine 0.7.3 OptionsState.hx hub: #EA71FD menuDesat, bold animated Alphabet
    // categories centered with flanking bold '>' and '<' selectors.
    void drawHub() {
        App& a = app();
        const PsychUi& ui = psychUi();
        gfx::beginCanvas();
        drawPsychMenuBackground(1, Color{234, 113, 253, 255}, catAnim_ * 10.0f, false, a.now, 0xEA71FD, menuPulse());
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 55});

        const int nc = int(cats_.size());
        const float step = nc <= 6 ? 86.0f : 74.0f;
        const float height = nc <= 6 ? 60.0f : 52.0f;
        const float topY = (gfx::H - 120.0f - float(nc - 1) * step - height) * 0.5f + 16.0f;
        for (int i = 0; i < nc; ++i) {
            const bool on = i == catSel_;
            const float y = topY + float(i) * step;
            const float alpha = on ? 1.0f : 0.6f;
            const std::string& title = cats_[size_t(i)].title;
            const float w = ui.alphabetCentered(title, gfx::W * 0.5f, y, height, alpha, true);
            if (w >= 0.0f) {
                if (on) {
                    const float leftEdge = gfx::W * 0.5f - w * 0.5f;
                    const float rightEdge = gfx::W * 0.5f + w * 0.5f;
                    ui.alphabet(">", leftEdge - 63.0f, y, height, 1.0f, true);
                    ui.alphabet("<", rightEdge + 15.0f, y, height, 1.0f, true);
                }
            } else {
                ui.uiText(title, gfx::W * 0.5f, y + 6.0f, on ? 42.0f : 34.0f, fade(on ? kGold : WHITE, alpha), 0, 2.0f);
                if (on) {
                    const float tw = ui.textWidth(title, 42.0f);
                    ui.uiText(">", gfx::W * 0.5f - tw * 0.5f - 36.0f, y + 6.0f, 38.0f, kGold, 0, 2.0f);
                    ui.uiText("<", gfx::W * 0.5f + tw * 0.5f + 36.0f, y + 6.0f, 38.0f, kGold, 0, 2.0f);
                }
            }
        }

        const OptionCategory& cur = cats_[size_t(catSel_)];
        DrawRectangleRec({90, gfx::H - 102, gfx::W - 180, 50}, Color{0, 0, 0, 155});
        ui.uiText(cur.subtitle, gfx::W * 0.5f, gfx::H - 89.0f, 22.0f, WHITE, 0, 1.5f);
        if (!a.touchMode) {
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 165});
            ui.uiText("UP/DOWN select   ENTER open   ESC save and back", gfx::W * 0.5f, gfx::H - 31.0f, 18.0f,
                      Color{220, 214, 235, 255}, 0, 1.25f);
        }
        gfx::endCanvas();
    }

    // Psych Engine 0.7.3 BaseOptionsMenu.hx: menuDesat (#EA71FD), top-left dimmed bold title,
    // slanted normal-Alphabet option labels, animated CheckboxThingie for booleans, bold Alphabet
    // values on the right, and semi-transparent bottom description box in vcr.ttf.
    void drawSection() {
        App& a = app();
        const PsychUi& ui = psychUi();
        const OptionCategory& cat = cats_[size_t(openSection_)];
        const std::string secTitle = inDelayCombo_ ? std::string("ADJUST DELAY AND COMBO") : cat.title;
        const auto& rows = activeRows();
        gfx::beginCanvas();
        drawPsychMenuBackground(1, Color{234, 113, 253, 255}, anim_ * 8.0f, false, a.now, 0xEA71FD, menuPulse());
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 95});

        // Subtle Psych BaseOptionsMenu top-left section title (scale ~0.6, alpha 0.45).
        if (ui.alphabet(secTitle, 64.0f, 24.0f, 40.0f, 0.48f, true) < 0)
            ui.uiText(secTitle, 64.0f, 26.0f, 34.0f, fade(WHITE, 0.55f), -1, 2.0f);

        const float rowStep = 86.0f;
        const float labelH = 52.0f;
        for (int i = 0; i < int(rows.size()); ++i) {
            const float d = float(i) - anim_;
            const float y = 290.0f + d * rowStep;
            if (y < 60.0f || y > gfx::H - 120.0f) continue;
            const Row& r = rows[size_t(i)];
            const bool on = i == sel_;
            const float alpha = on ? 1.0f : 0.6f;
            const bool isBool = r.kind == Row::Bool;
            const float baseX = (isBool ? 290.0f : 195.0f) + d * 18.0f;

            if (isBool) {
                const bool checked = r.label == "Fullscreen" ? a.settings.fullscreen : *r.b;
                const double el = (lastToggleRow_ == i) ? (a.now - lastToggleAt_) : -1.0;
                if (!ui.drawCheckbox({baseX - 112.0f, y - 14.0f}, checked, el, 0.72f, alpha))
                    ui.uiText(checked ? "[X]" : "[ ]", baseX - 80.0f, y + 8.0f, 32.0f, fade(WHITE, alpha), 0, 2.0f);
            }

            // Option label in normal (non-bold) Alphabet, matching Psych BaseOptionsMenu.hx.
            float lw = ui.alphabet(r.label, baseX, y, labelH, alpha, false);
            if (lw < 0.0f) {
                ui.uiText(r.label, baseX, y + 10.0f, 30.0f, fade(WHITE, alpha), -1, 2.0f);
                lw = ui.textWidth(r.label, 30.0f);
            }

            if (r.kind == Row::Key) {
                for (int k = 0; k < 2; ++k) {
                    const bool hl = on && k == slot_;
                    const std::string name = (hl && waiting_) ? "press a key..." : keyName(a.settings.keys4[r.lane][k]);
                    const float x = 840.0f + k * 180.0f;
                    DrawRectangleRec({x - 80.0f, y + 2.0f, 160.0f, 46.0f}, hl ? Color{255, 255, 255, 78} : Color{0, 0, 0, 150});
                    ui.uiText(name, x, y + 12.0f, 24.0f, fade(hl ? kGold : WHITE, alpha), 0, 1.5f);
                }
                gfx::drawNote(r.lane, {710.0f, y + 24.0f}, 46.0f, alpha);
                continue;
            }
            if (r.kind == Row::AsmCpu) {
                const int activeOpt = clampv(a.settings.noteAsmCpu, 0, 2);
                const float x0 = 620.0f;
                const float wChip = 190.0f;
                for (int k = 0; k < 3; ++k) {
                    const auto opt = static_cast<raster::CpuAsmOption>(k);
                    const bool enabled = raster::cpuAsmEnabled(opt);
                    const bool chosen = enabled && (activeOpt == k);
                    const float cx = x0 + float(k) * (wChip + 10.0f);
                    Color bg = chosen ? Color{70, 115, 70, 210} : (enabled ? Color{30, 30, 42, 185} : Color{22, 22, 28, 170});
                    DrawRectangleRec({cx, y + 4.0f, wChip, 42.0f}, bg);
                    DrawRectangleLinesEx({cx, y + 4.0f, wChip, 42.0f}, chosen ? 2.0f : 1.0f,
                                         chosen ? kGold : (enabled ? fade(WHITE, 0.45f) : fade(WHITE, 0.16f)));
                    const std::string text = enabled ? std::string(raster::cpuAsmLabel(opt))
                                                     : (std::string(raster::cpuAsmLabel(opt)) + " [OFF]");
                    const Color tc = chosen ? kGold : (enabled ? WHITE : Color{135, 135, 148, 255});
                    ui.uiText(text, cx + wChip * 0.5f, y + 15.0f, 18.0f, fade(tc, alpha), 0, 1.25f);
                }
                continue;
            }
            if (isBool) continue;

            std::string val;
            switch (r.kind) {
                case Row::Number: val = r.fmt(*r.f); break;
                case Row::Choice:
                    if (!r.names.empty()) {
                        for (size_t k = 0; k < r.choices.size() && k < r.names.size(); ++k)
                            if (r.choices[k] == *r.i) val = r.names[k];
                    } else {
                        val = *r.i == 0 ? std::string("Unlimited") : std::string(TextFormat("%d FPS", *r.i));
                    }
                    break;
                case Row::Action: val = r.value ? r.value() : std::string(">"); break;
                default: break;
            }
            if (!val.empty()) {
                const float vx = baseX + lw + 44.0f;
                const float maxValW = std::max(140.0f, gfx::W - 44.0f - vx);
                float valH = 50.0f;
                const float rawW = ui.alphabetWidth(val, valH, true);
                if (rawW > 0.0f) {
                    if (rawW > maxValW) valH *= maxValW / rawW;
                    const float vy = y + (labelH - valH) * 0.5f;
                    ui.alphabet(val, vx, vy, valH, alpha, true, on ? WHITE : Color{230, 230, 240, 255});
                } else {
                    ui.uiText(val, vx, y + 10.0f, 30.0f, fade(on ? kGold : WHITE, alpha), -1, 2.0f);
                }
            }
        }

        const Row& r = rows[size_t(sel_)];
        DrawRectangleRec({100, gfx::H - 114, gfx::W - 200, 66}, Color{0, 0, 0, 155});
        const std::vector<std::string> helpLines = wrapWidth(ui, true, FontId::Ui, r.help, 24.0f, gfx::W - 240.0f);
        for (size_t li = 0; li < std::min<size_t>(2, helpLines.size()); ++li)
            ui.uiText(helpLines[li], gfx::W * 0.5f, gfx::H - 104.0f + float(li) * 26.0f, 24.0f, WHITE, 0, 1.5f);
        if (!a.touchMode) {
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 165});
            ui.uiText(inDelayCombo_ ? "UP/DOWN select   LEFT/RIGHT change   ENTER open   ESC back to Gameplay"
                                    : "UP/DOWN select   LEFT/RIGHT change   ENTER toggle   ESC back",
                      gfx::W * 0.5f, gfx::H - 31.0f, 18.0f, Color{220, 214, 235, 255}, 0, 1.25f);
        }
        gfx::endCanvas();
    }

    void buildCategories() {
        Settings& s = app().settings;
        auto pct = [](float v) { return std::string(TextFormat("%d%%", int(std::lround(v * 100)))); };
        cats_.clear();

        // Nested sub-page hidden under Gameplay: Adjust Delay and Combo.
        delayComboRows_ = {
            {Row::Action, "Calibrate offset", "Tap along to a metronome to measure your audio/visual offset."},
            {Row::Number, "Audio offset", "Positive if you hear the music late. Hold SHIFT for steps of 10 ms.", nullptr,
             &s.offsetMs, -300, 300, 1},
            {Row::Choice, "Audio latency", "Audio buffer size: lower is faster; raise if sound crackles (next start).", nullptr, nullptr, 0, 0,
             0, &s.audioLatency, {0, 1, 2, 3}, {"Lowest (6 ms)", "Low (15 ms)", "Normal (30 ms)", "Safe (60 ms)"}},
            {Row::Number, "Rating X", "Horizontal offset for Sick/Good/Bad/Shit popups.", nullptr, &s.comboOffset[0], -600, 600, 1},
            {Row::Number, "Rating Y", "Vertical offset for Sick/Good/Bad/Shit popups.", nullptr, &s.comboOffset[1], -400, 400, 1},
            {Row::Number, "Combo X", "Horizontal offset for combo digit popups.", nullptr, &s.comboOffset[2], -600, 600, 1},
            {Row::Number, "Combo Y", "Vertical offset for combo digit popups.", nullptr, &s.comboOffset[3], -400, 400, 1},
        };
        for (Row& r : delayComboRows_) {
            if (r.label == "Calibrate offset") r.action = [] { app().change(makeCalibration()); };
            else if (r.label == "Audio offset") r.fmt = [](float v) { return std::string(TextFormat("%+d ms", int(std::lround(v)))); };
            else if (r.kind == Row::Number) r.fmt = [](float v) { return std::string(TextFormat("%+d", int(std::lround(v)))); };
        }

        // 1. Note Colors (direct screen, just like Psych Engine)
        cats_.push_back({"NOTE COLORS", "Customize the main, highlight and outline colors for each note direction.",
                         [] { app().change(makeNoteColors()); }, {}});

        // 2. Controls
        {
            OptionCategory c{"CONTROLS", "Rebind 4-key lanes or configure multi-key (1 to 18 keys) bindings.", {}, {}};
            c.rows = {
                {Row::Key, "Left lane", "ENTER to rebind, LEFT/RIGHT to pick a slot, BACKSPACE to clear."},
                {Row::Key, "Down lane", "ENTER to rebind, LEFT/RIGHT to pick a slot, BACKSPACE to clear."},
                {Row::Key, "Up lane", "ENTER to rebind, LEFT/RIGHT to pick a slot, BACKSPACE to clear."},
                {Row::Key, "Right lane", "ENTER to rebind, LEFT/RIGHT to pick a slot, BACKSPACE to clear."},
                {Row::Action, "Reset 4-key controls", "Restore D F J K and the arrow keys."},
                {Row::Action, "Key bindings (1-18K)", "Configure every lane key for 1 to 18 key charts."},
            };
            int lane = 0;
            for (Row& r : c.rows) {
                if (r.kind == Row::Key) r.lane = lane++;
                if (r.label == "Reset 4-key controls")
                    r.action = [&s] {
                        const Settings d;
                        for (int l = 0; l < 4; ++l)
                            for (int k = 0; k < 2; ++k) s.keys4[l][k] = d.keys4[l][k];
                        app().saveSettings();
                    };
                if (r.label == "Key bindings (1-18K)") r.action = [] { app().change(makeKeyBindings()); };
            }
            cats_.push_back(std::move(c));
        }

        // 3. Graphics
        {
            OptionCategory c{"GRAPHICS", "Display, framerate, ASM note renderer CPU target, shaders, and FPS overlay.", {}, {}};
            c.rows = {
                {Row::Bool, "Fullscreen", "Borderless fullscreen (F11 works anywhere).", &s.fullscreen},
                {Row::Bool, "VSync", "Waits for display refresh. OFF uses Frame limit below for lowest input latency.", &s.vsync},
                {Row::Bool, "Low latency", "With VSync: paces frame submission right before display refresh.", &s.lowLatency},
                {Row::Choice, "Frame limit", "Higher limits make note scrolling smoother. Input is polled independently.", nullptr, nullptr,
                 0, 0, 0, &s.fpsCap, {60, 120, 144, 165, 240, 360, 500, 0}},
                {Row::Bool, "Anti-aliasing", "Smooth scaling of sprites. OFF uses sharp pixels and runs slightly faster.", &s.antialiasing},
                {Row::Bool, "Shaders", "Mod shaders and screen effects. OFF is faster on older GPUs.", &s.shaders},
                {Row::Bool, "Low detail", "Fewer particles and effects for slower machines.", &s.lowDetail},
                {Row::Choice, "Note detail", "Spam charts: Adaptive collapses stacked notes when FPS drops; Full draws every pixel.", nullptr,
                 nullptr, 0, 0, 0, &s.noteDetail, {0, 1}, {"Adaptive", "Full"}},
                {Row::Choice, "Note renderer", "Auto picks GPU unless software OpenGL is detected. CPU (ASM) uses SIMD assembly blending.", nullptr,
                 nullptr, 0, 0, 0, &s.noteRenderer, {3, 0, 1, 2}, {"Auto", "GPU", "CPU (ASM)", "CPU (C++)"}},
                {Row::AsmCpu, "ASM CPU target",
                 "ASM note renderer CPU target: x86 SSE2 (active), x86-64 SSE4 (disabled), AVX2 (disabled)."},
                {Row::Bool, "Show FPS", "Display the frame rate counter in the top-left corner.", &s.showFps},
                {Row::Bool, "FPS memory", "Show process memory usage and peak under FPS.", &s.fpsMemory},
                {Row::Bool, "FPS frame times", "Show update/draw timings and frame-time graph under FPS.", &s.fpsFrameTimes},
                {Row::Bool, "FPS CPU/GPU/RAM", "Show CPU/GPU load and memory usage under FPS.", &s.fpsUsage},
                {Row::Bool, "FPS hardware", "Show CPU, GPU, OpenGL version and OS under FPS.", &s.fpsHardware},
            };
            if (platform::kAndroid) {
                c.rows.erase(std::remove_if(c.rows.begin(), c.rows.end(), [](const Row& r) { return r.label == "Fullscreen"; }), c.rows.end());
            }
            cats_.push_back(std::move(c));
        }

        // 4. Visuals and UI (Psych UI toggles are intentionally hidden; bundled Psych assets are always active)
        {
            OptionCategory c{"VISUALS AND UI", "Note skins, HUD visibility, time bar, splashes, and pause music.", {}, {}};
            c.rows = {
                {Row::Action, "Note skin", "Choose between Default and bundled/mod note skins."},
                {Row::Bool, "Note splashes", "Bursts of light on SICK! hits.", &s.noteSplashes},
                {Row::Number, "Splash opacity", "Opacity of note splash bursts.", nullptr, &s.splashAlpha, 0, 1, 0.1f},
                {Row::Bool, "Hide HUD", "Hides health bar, icons, score text and ratings during gameplay.", &s.hideHud},
                {Row::Choice, "Time bar", "What the song progress bar at the top shows.", nullptr, nullptr, 0, 0, 0, &s.timeBar,
                 {0, 1, 2, 3}, {"Time left", "Time elapsed", "Song name", "Off"}},
                {Row::Bool, "Flashing lights", "Enable flashing lights and bright beat flashes.", &s.flashing},
                {Row::Bool, "Camera zooms", "Beat camera bops (chart/script camera zooms always work).", &s.camZoom},
                {Row::Bool, "Score text zoom", "Bounce score text slightly on each note hit.", &s.scoreZoom},
                {Row::Number, "Health bar alpha", "Opacity of the health bar and icons.", nullptr, &s.healthBarAlpha, 0, 1, 0.1f},
                {Row::Bool, "Hit popups", "Show rating images and combo numbers on each hit.", &s.hitPopups},
                {Row::Bool, "Combo stacking", "Stack rating and combo popups instead of replacing the previous one.", &s.comboStacking},
                {Row::Choice, "Note counter", "Show notes hit under the score line.", nullptr, nullptr, 0, 0, 0, &s.noteCounter, {0, 1, 2},
                 {"Off", "Your side", "Both sides"}},
                {Row::Bool, "Audio visualizer", "Show reactive audio spectrum bars in the top-right corner of songs.", &s.visualizer},
                {Row::Bool, "Botplay text", "Show BOTPLAY / REPLAY indicator while auto-playing.", &s.botplayText},
                {Row::Bool, "Menu bg blur", "Blur menu backgrounds when shaders are enabled.", &s.menuBlur},
                {Row::Choice, "Pause music", "Music played on the pause screen.", nullptr, nullptr, 0, 0, 0, &s.pauseMusic, {0, 1, 2},
                 {"None", "Breakfast", "Tea Time"}},
            };
            for (Row& r : c.rows) {
                if (r.kind == Row::Number) r.fmt = pct;
                if (r.label == "Note skin") {
                    r.action = [] { app().change(makeNoteSkins()); };
                    r.value = [&s] { return s.noteSkin.empty() ? std::string("Default") : s.noteSkin; };
                }
            }
            cats_.push_back(std::move(c));
        }

        // 5. Gameplay (includes hidden "Adjust Delay and Combo" submenu)
        {
            OptionCategory c{"GAMEPLAY", "Scroll direction, middlescroll, ghost tapping, hit windows, delay & combo.", {}, {}};
            c.rows = {
                {Row::Bool, "Downscroll", "Notes scroll downward toward receptors at the bottom.", &s.downscroll},
                {Row::Bool, "Middlescroll", "Center player lanes and dim opponent lanes.", &s.middlescroll},
                {Row::Bool, "Opponent notes", "Show opponent receptors and incoming notes.", &s.opponentNotes},
                {Row::Bool, "Opponent strums", "Light up opponent receptors when the opponent hits notes.", &s.opponentStrums},
                {Row::Bool, "Ghost tapping", "Pressing lanes with no note nearby does not count as a miss.", &s.ghostTapping},
                {Row::Bool, "Auto pause", "Pause gameplay automatically when the window loses focus.", &s.autoPause},
                {Row::Bool, "Disable reset", "Prevent R from instantly triggering game over during a song.", &s.noReset},
                {Row::Bool, "Extra keys", "Allow 1 to 18 key charts and mid-song key count changes (OFF forces 4K).", &s.extraKeys},
                {Row::Bool, "Touch controls", "Enable on-screen touch lane buttons.", &s.touchControls},
                {Row::Number, "Scroll speed", "Override chart scroll speed (0 = use chart speed).", nullptr, &s.scrollSpeed, 0, 6, 0.1f},
                {Row::Bool, "Show timing (ms)", "Display hit error in milliseconds on each note.", &s.showTimings},
                {Row::Action, "Adjust Delay and Combo", "Calibrate audio offset, buffer latency, and rating/combo popup positions."},
                {Row::Number, "Rating offset", "Shifts the center of judgement windows in milliseconds.", nullptr, &s.ratingOffset, -30, 30, 1},
                {Row::Number, "Sick! window", "Maximum timing error (ms) for a SICK! rating.", nullptr, &s.sickWindow, 15, 45, 1},
                {Row::Number, "Good window", "Maximum timing error (ms) for a GOOD rating.", nullptr, &s.goodWindow, 15, 90, 1},
                {Row::Number, "Bad window", "Maximum timing error (ms) for a BAD rating.", nullptr, &s.badWindow, 15, 135, 1},
                {Row::Number, "Safe frames", "Maximum early/late hit window in 60 FPS frames.", nullptr, &s.safeFrames, 2, 10, 0.1f},
            };
            if (platform::kAndroid) {
                c.rows.erase(std::remove_if(c.rows.begin(), c.rows.end(), [](const Row& r) { return r.label == "Touch controls"; }), c.rows.end());
            }
            for (Row& r : c.rows) {
                if (r.label == "Adjust Delay and Combo") {
                    r.action = [this] {
                        inDelayCombo_ = true;
                        gLastInDelayCombo = true;
                        sel_ = 0;
                        anim_ = 0;
                        lastToggleRow_ = -1;
                        if (!delayComboRows_.empty()) gLastOptionsRow = delayComboRows_[0].label;
                    };
                } else if (r.label == "Scroll speed") {
                    r.fmt = [](float v) { return v <= 0.001f ? std::string("Chart") : std::string(TextFormat("%.1f", v)); };
                } else if (r.label == "Rating offset") {
                    r.fmt = [](float v) { return std::string(TextFormat("%+d ms", int(std::lround(v)))); };
                } else if (r.label.find("window") != std::string::npos) {
                    r.fmt = [](float v) { return std::string(TextFormat("%d ms", int(std::lround(v)))); };
                } else if (r.label == "Safe frames") {
                    r.fmt = [](float v) { return std::string(TextFormat("%.1f", v)); };
                }
            }
            cats_.push_back(std::move(c));
        }

        // 6. Audio
        {
            OptionCategory c{"AUDIO", "Master, music, SFX and hitsound volume plus hitsound polyphony.", {}, {}};
            c.rows = {
                {Row::Number, "Master volume", "Overall output volume.", nullptr, &s.masterVol, 0, 1, 0.05f},
                {Row::Number, "Music volume", "Song and menu music volume.", nullptr, &s.musicVol, 0, 1, 0.05f},
                {Row::Number, "Sound effects", "Menu and miss sound effects volume.", nullptr, &s.sfxVol, 0, 1, 0.05f},
                {Row::Number, "Hit sounds", "Sample-accurate click on every hit note.", nullptr, &s.hitsoundVol, 0, 1, 0.05f},
                {Row::Choice, "Hitsound mode", "How overlapping hitsounds behave on dense chords/spam.", nullptr, nullptr, 0, 0, 0,
                 &s.hitsoundPolyphony, {0, 1, 2}, {"Off (cut self)", "Polyphonic", "Unlimited"}},
                {Row::Bool, "Hitsound limiter", "Soft-clips stacked hitsounds to prevent harsh distortion.", &s.hitsoundLimiter},
            };
            for (Row& r : c.rows)
                if (r.kind == Row::Number) r.fmt = pct;
            cats_.push_back(std::move(c));
        }

        // 7. Storage (Android configurable storage path, defaulting to /storage/emulated/0/.astralis)
        if (platform::kAndroid) {
            OptionCategory c{"STORAGE", "Choose where Astralis stores mods, songs, settings and saves on Android.", {}, {}};
            c.rows = {
                {Row::Action, "Storage folder", "Tap to enter a custom folder path (default: /storage/emulated/0/.astralis)."},
                {Row::Action, "Default (.astralis)", "Switch back to /storage/emulated/0/.astralis."},
                {Row::Action, "App-private folder", "Use Android/data/<package>/files (requires no storage permission)."},
                {Row::Action, "Storage permission", "Grant All Files Access so Astralis can read/write /storage/emulated/0/.astralis."},
            };
            c.rows[0].value = [] { return platform::displayPath(platform::configuredStorageDir()); };
            c.rows[0].action = [this] {
                if (platform::askText("Storage folder", platform::configuredStorageDir())) askingStorage_ = true;
            };
            c.rows[1].action = [this] { applyStoragePath(platform::defaultStorageDir()); };
            c.rows[2].action = [this] { applyStoragePath(platform::privateStorageDir()); };
            c.rows[3].value = [] { return platform::hasExternalStorageAccess() ? std::string("Granted") : std::string("Tap to allow"); };
            c.rows[3].action = [] {
                platform::requestExternalStorageAccess();
                app().notify("Allow storage access, then restart or reopen this menu", 4.0);
            };
            cats_.push_back(std::move(c));
        }

        // 8. Debug & Video Render
        {
            OptionCategory c{"DEBUG", "Lua script diagnostics, note callbacks, and video render settings.", {}, {}};
            c.rows = {
                {Row::Choice, "Lua errors", "When to display mod script errors on screen.", nullptr, nullptr, 0, 0, 0, &s.luaErrors, {0, 1, 2},
                 {"Always", "luaDebugMode", "Off"}},
                {Row::Choice, "Lua warnings", "When to display missing asset warnings from scripts.", nullptr, nullptr, 0, 0, 0, &s.luaWarnings,
                 {0, 1, 2}, {"Always", "luaDebugMode", "Off"}},
                {Row::Bool, "Lua note hooks", "Forward plain note hits/misses to Lua scripts (OFF is faster on spam charts).",
                 &s.luaNoteCallbacks},
                {Row::Choice, "Render size", "Resolution when recording via --render (desktop renders/ or Android Movies/Astralis).",
                 nullptr, nullptr, 0, 0, 0, &s.renderSize, {0, 1, 2, 3}, {"1280x720", "1920x1080", "2560x1440", "3840x2160"}},
                {Row::Choice, "Render FPS", "Target FPS when recording videos.", nullptr, nullptr, 0, 0, 0, &s.renderFps, {30, 60, 120, 240}},
                {Row::Choice, "Render encoder", "Video codec/hardware encoder for ffmpeg.", nullptr, nullptr, 0, 0, 0, &s.renderEncoder,
                 {7, 0, 1, 2, 3, 4, 5, 6},
                 {"Auto", "H.264 (x264)", "H.265 (x265)", "VP9 (WebM)", "H.264 NVIDIA", "H.265 NVIDIA", "H.264 AMD", "H.264 Intel"}},
                {Row::Choice, "Render bitrate", "Video bitrate for recorded runs.", nullptr, nullptr, 0, 0, 0, &s.renderBitrate,
                 {5, 10, 20, 40, 80}, {"5 Mbps", "10 Mbps", "20 Mbps", "40 Mbps", "80 Mbps"}},
                {Row::Bool, "Render audio", "Include song audio in recorded videos.", &s.renderAudio},
            };
            if (platform::kAndroid) {
                c.rows.erase(std::remove_if(c.rows.begin(), c.rows.end(), [](const Row& r) { return r.label == "Render encoder"; }), c.rows.end());
            }
            cats_.push_back(std::move(c));
        }
    }

    std::vector<OptionCategory> cats_;
    std::vector<Row> delayComboRows_;
    int catSel_ = 0;
    float catAnim_ = 0;
    int openSection_ = -1;  // -1 = Psych-style Options Hub
    bool inDelayCombo_ = false;
    int sel_ = 0, slot_ = 0;
    float anim_ = 0;
    int lastToggleRow_ = -1;
    double lastToggleAt_ = -10.0;
    bool waiting_ = false;
    bool askingStorage_ = false;
};

// ----------------------------------------------------------- Key bindings --
// The keys of every key count, 1 to 18: the lanes as a song shows them, each
// with its key. LEFT/RIGHT pick a lane (on the top row, the key count), ENTER
// or a tap on a lane waits for its new key, BACKSPACE gives the lane its
// default back, R all of them.

class KeyBindState : public State {
public:
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.keys = {{"FEWER KEYS", KEY_PAGE_DOWN}, {"MORE KEYS", KEY_PAGE_UP}, {"RESET LANE", KEY_DELETE}, {"RESET ALL", KEY_R}};
        return t;
    }
    bool keyReserved(int) const override { return waiting_; }  // the next key is the binding

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        Settings& s = a.settings;
        anim_ = approach(anim_, float(lane_), 14.0f, float(dt));
        if (waiting_) {
            const int code = in.firstPressed();
            if (code == KEY_ESCAPE || code == KEY_BACK) {
                waiting_ = false;
                sfxPlay(Sfx::Cancel);
            } else if (code > 0 && code < kPadBase) {
                setKey(s, lane_, code);
                waiting_ = false;
                sfxPlay(Sfx::Confirm);
                a.saveSettings();
            }
            return;
        }
        const int before = keys_;
        if (in.pressed(KEY_PAGE_UP) || (row_ == 0 && in.right())) keys_ = std::min(ek::kTableKeys, keys_ + 1);
        if (in.pressed(KEY_PAGE_DOWN) || (row_ == 0 && in.left())) keys_ = std::max(1, keys_ - 1);
        if (keys_ != before) {
            lane_ = clampv(lane_, 0, keys_ - 1);
            sfxPlay(Sfx::Scroll);
        }
        if (in.up() && row_ == 1) { row_ = 0; sfxPlay(Sfx::Scroll); }
        if (in.down() && row_ == 0) { row_ = 1; sfxPlay(Sfx::Scroll); }
        if (row_ == 1) {
            if (in.left()) { lane_ = (lane_ + keys_ - 1) % keys_; sfxPlay(Sfx::Scroll); }
            if (in.right()) { lane_ = (lane_ + 1) % keys_; sfxPlay(Sfx::Scroll); }
        }
        if (in.accept()) {
            if (row_ == 0) row_ = 1;
            else waiting_ = true;
            sfxPlay(Sfx::Scroll);
        }
        if (in.pressed(KEY_DELETE) || (in.pressed(KEY_BACKSPACE) && row_ == 1)) {
            setKey(s, lane_, 0);
            sfxPlay(Sfx::Cancel);
            a.saveSettings();
            return;
        }
        if (in.pressed(KEY_R)) {
            for (int l = 0; l < keys_; ++l) setKey(s, l, 0);
            sfxPlay(Sfx::Confirm);
            a.saveSettings();
        }
        // A tap or click on a lane: that lane waits for its key.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const Vector2 p = gfx::toCanvas(GetMousePosition());
            for (int l = 0; l < keys_; ++l) {
                const Vector2 c = laneCenter(l);
                if (std::fabs(p.x - c.x) < laneStep() * 0.5f && std::fabs(p.y - c.y) < 90) {
                    lane_ = l;
                    row_ = 1;
                    waiting_ = true;
                    sfxPlay(Sfx::Scroll);
                    return;
                }
            }
        }
        if (in.back() && !in.pressed(KEY_BACKSPACE)) {
            a.saveSettings();
            sfxPlay(Sfx::Cancel);
            a.change(makeOptions());
        }
    }

    void draw() override {
        App& a = app();
        const Settings& s = a.settings;
        const bool psych = a.psychMenusActive();
        const PsychUi& ui = psychUi();
        gfx::beginCanvas();
        if (psych) {
            drawPsychMenuBackground(1, Color{234, 113, 253, 255}, 0, false, a.now, 0xEA71FD, menuPulse());
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 145});
            if (ui.alphabet("KEY BINDINGS", 56.0f, 16.0f, 52.0f, 1.0f) < 0)
                ui.uiText("KEY BINDINGS", 56.0f, 18.0f, 44.0f, WHITE, -1, 2.5f);
        } else {
            drawMenuBackground(a.now, 0x5C9CFF, menuPulse());
            gfx::text(FontId::Title, "KEY BINDINGS", 60, 22, 48, kGold, Align::Left, 2.0f);
        }
        const bool top = row_ == 0;
        DrawRectangleRounded({440, 110, 400, 56}, 0.4f, 8, top ? Color{255, 214, 120, 60} : Color{0, 0, 0, 130});
        if (psych) ui.uiText(TextFormat("<   %d KEY%s   >", keys_, keys_ == 1 ? "" : "S"), 640.0f, 121.0f, 32.0f, top ? kGold : WHITE, 0, 2.0f);
        else gfx::text(FontId::Title, TextFormat("<   %d KEY%s   >", keys_, keys_ == 1 ? "" : "S"), 640, 116, 38, top ? kGold : kIvory,
                       Align::Center, 2.0f);
        const auto lanes = s.laneKeys(keys_);
        const auto defaults = Settings::defaultLaneKeys(keys_);
        const float size = std::min(96.0f, laneStep() * 0.9f);
        for (int l = 0; l < keys_; ++l) {
            const Vector2 c = laneCenter(l);
            const bool on = row_ == 1 && l == lane_;
            if (on) DrawRectangleRounded({c.x - laneStep() * 0.5f + 2, c.y - 70, laneStep() - 4, 190}, 0.2f, 8, Color{255, 214, 120, 50});
            gfx::drawNote(gfx::laneDir(keys_, l), {c.x, c.y}, size, on ? 1.0f : 0.75f);
            const int code = l < int(lanes.size()) && !lanes[size_t(l)].empty() ? lanes[size_t(l)][0] : 0;
            const bool changed = l < int(defaults.size()) && !defaults[size_t(l)].empty() && code != defaults[size_t(l)][0];
            const std::string name = on && waiting_ ? std::string("?") : std::string(code ? keyName(code) : "-");
            const float ts = keys_ > 12 ? 20.0f : keys_ > 8 ? 24.0f : 30.0f;
            if (psych) ui.uiText(name, c.x, c.y + 64, ts, on ? WHITE : (changed ? kGold : kIvory), 0, 1.5f);
            else gfx::text(FontId::Ui, name, c.x, c.y + 64, ts, on ? WHITE : (changed ? kGold : kIvory), Align::Center, 1.5f);
        }
        std::string help = waiting_ ? "Press the key for this lane (ESC: keep the one it has)"
                                    : "Lanes for charts with this many keys. Gold: a key you chose.";
        if (keys_ == 4 && !waiting_) help = "4 keys: also in Options > CONTROLS, with a second key for each lane.";
        DrawRectangleRec({0, gfx::H - 84, gfx::W, 44}, Color{0, 0, 0, 140});
        if (psych) ui.uiText(help, gfx::W * 0.5f, gfx::H - 74.0f, 22.0f, WHITE, 0, 1.5f);
        else gfx::text(FontId::Ui, help, gfx::W / 2, gfx::H - 76, 24, kIvory, Align::Center);
        if (!a.touchMode) {
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 165});
            const char* kh = "UP/DOWN row   LEFT/RIGHT keys or lane   ENTER rebind   BACKSPACE default   R all defaults   ESC back";
            if (psych) ui.uiText(kh, gfx::W * 0.5f, gfx::H - 31.0f, 18.0f, Color{220, 214, 235, 255}, 0, 1.25f);
            else gfx::text(FontId::Ui, kh, gfx::W / 2, gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
        }
        gfx::endCanvas();
    }

private:
    float laneStep() const { return std::min(130.0f, 1180.0f / float(keys_)); }
    Vector2 laneCenter(int lane) const {
        const float w = laneStep() * float(keys_);
        return {640.0f - w * 0.5f + laneStep() * (float(lane) + 0.5f), 330.0f};
    }
    void setKey(Settings& s, int lane, int code) {
        if (keys_ == 4) {
            const Settings d;
            s.keys4[lane][0] = code ? code : d.keys4[lane][0];
            return;
        }
        std::vector<int>& v = s.laneBinds[keys_];
        v.resize(size_t(keys_), 0);
        v[size_t(lane)] = code;
    }
    int keys_ = 9, lane_ = 0, row_ = 1;
    float anim_ = 0;
    bool waiting_ = false;
};

// ------------------------------------------------------------ Calibration --

class CalibrationState : public State {
public:
    bool wantsMenuMusic() const override { return false; }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        t.tapCode = KEY_SPACE;  // tap anywhere on the beat
        t.keys = {{"APPLY", KEY_ENTER}, {"RESET", KEY_R}, {"CANCEL", KEY_ESCAPE}};
        return t;
    }
    void enter() override {
        App& a = app();
        a.stopMenuMusic();
        // 16 beats at 120 BPM, accent on every bar.
        synth::Stereo s(size_t(8.0 * synth::SR));
        for (int b = 0; b < 16; ++b) {
            synth::bell(s, nullptr, b * 0.5, b % 4 == 0 ? 88 : 81, 0.9f, 0);
            synth::kick(s, b * 0.5, 0.5f, 0.6f);
        }
        auto pcm = std::make_shared<Pcm>();
        pcm->channels = 2;
        pcm->data = synth::toPcm16(s, 0.8f);
        a.mixer.setTracks(pcm, nullptr, nullptr);
        a.mixer.setRate(1.0);
        a.mixer.setLoop(true, 0, 8.0);
        a.mixer.play(0);
    }
    void update(double) override {
        App& a = app();
        const double clock = a.mixer.time(a.now) * 1000.0;
        for (const InputEvent& e : a.input.events()) {
            if (!e.down) continue;
            if (e.code == KEY_ESCAPE || e.code == KEY_BACKSPACE) {
                sfxPlay(Sfx::Cancel);
                a.change(makeOptions());
                return;
            }
            if (e.code == KEY_ENTER) {
                if (taps_.size() >= 4) {
                    a.settings.offsetMs = float(std::lround(mean()));
                    a.saveSettings();
                    sfxPlay(Sfx::Confirm);
                }
                a.change(makeOptions());
                return;
            }
            if (e.code == KEY_R) { taps_.clear(); continue; }
            const double t = clock - (a.now - e.t) * 1000.0;
            const double k = std::round(t / 500.0);
            const double diff = t - k * 500.0;
            taps_.push_back(diff);
            if (taps_.size() > 24) taps_.erase(taps_.begin());
            lastTap_ = a.now;
        }
        clock_ = clock;
    }
    void draw() override {
        App& a = app();
        const bool psych = a.psychMenusActive();
        const PsychUi& ui = psychUi();
        gfx::beginCanvas();
        if (psych) {
            drawPsychMenuBackground(1, Color{92, 200, 255, 255}, 0, false, a.now, 0x5CC8FF, 0);
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 145});
            if (ui.alphabetCentered("CALIBRATE OFFSET", gfx::W * 0.5f, 28.0f, 52.0f, 1.0f) < 0)
                ui.uiText("CALIBRATE OFFSET", gfx::W * 0.5f, 32.0f, 44.0f, WHITE, 0, 2.5f);
            ui.uiText(a.touchMode ? "Tap the screen on every beat you HEAR (not see). Keep going for a few bars."
                                  : "Tap any key on every beat you HEAR (not see). Keep going for a few bars.",
                      gfx::W * 0.5f, 112.0f, 22.0f, WHITE, 0, 1.5f);
        } else {
            drawMenuBackground(a.now, 0x5CC8FF, 0);
            gfx::text(FontId::Title, "CALIBRATE OFFSET", gfx::W / 2, 40, 48, kGold, Align::Center, 2.0f);
            gfx::text(FontId::Ui,
                      a.touchMode ? "Tap the screen on every beat you HEAR (not see). Keep going for a few bars."
                                  : "Tap any key on every beat you HEAR (not see). Keep going for a few bars.",
                      gfx::W / 2, 110, 28, kIvory, Align::Center, 1.5f);
        }
        // visual metronome, compensated with the current offset
        const double vis = clock_ - a.settings.offsetMs;
        const double frac = std::fmod(vis / 500.0 + 1000.0, 1.0);
        const float pulse = float(std::exp(-frac * 7.0));
        gfx::glow({640, 330}, 120 + 60 * pulse, fade(kGold, 0.4f + 0.4f * pulse));
        DrawCircleV({640, 330}, 50 + 14 * pulse, fade(kGold, 0.85f));
        gfx::drawReceptor(gfx::kDown, {640, 520}, 90, WHITE);
        for (int k = -1; k < 6; ++k) {
            const double beatT = (std::floor(vis / 500.0) + k) * 500.0;
            const float y = 520 + float(beatT - vis) * 0.45f * 2.0f;
            if (y > 520 - 60 && y < 760) gfx::drawNote(gfx::kDown, {640, y}, 90);
        }
        std::string res = "Taps: 0";
        if (!taps_.empty()) {
            double sd = 0;
            const double m = mean();
            for (double t : taps_) sd += (t - m) * (t - m);
            sd = std::sqrt(sd / double(taps_.size()));
            res = TextFormat("Average: %+.0f ms    Spread: %.0f ms    Taps: %d", m, sd, int(taps_.size()));
        }
        if (psych) {
            ui.uiText(res, gfx::W * 0.5f, 410.0f, 30.0f, WHITE, 0, 2.0f);
            ui.uiText(TextFormat("Current offset: %+d ms", int(a.settings.offsetMs)), gfx::W * 0.5f, 452.0f, 22.0f, fade(WHITE, 0.75f), 0, 1.5f);
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 165});
            ui.uiText(a.touchMode ? "APPLY needs 4+ taps" : "ENTER apply (needs 4+ taps)   R reset   ESC cancel", gfx::W * 0.5f,
                      gfx::H - 31.0f, 18.0f, Color{220, 214, 235, 255}, 0, 1.25f);
        } else {
            gfx::text(FontId::Ui, res, gfx::W / 2, 410, 34, WHITE, Align::Center, 1.5f);
            gfx::text(FontId::Ui, TextFormat("Current offset: %+d ms", int(a.settings.offsetMs)), gfx::W / 2, 450, 24,
                      fade(kIvory, 0.7f), Align::Center);
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
            gfx::text(FontId::Ui, a.touchMode ? "APPLY needs 4+ taps" : "ENTER apply (needs 4+ taps)   R reset   ESC cancel", gfx::W / 2,
                      gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
        }
        gfx::endCanvas();
    }

private:
    double mean() const {
        double s = 0;
        for (double t : taps_) s += t;
        return taps_.empty() ? 0 : s / double(taps_.size());
    }
    std::vector<double> taps_;
    double clock_ = 0, lastTap_ = 0;
};


// ------------------------------------------------------------ note colors --

// Psych's Note Colors: each direction's main colour, highlight and outline
// (the three channels of Psych 0.7's colour-template note skins), changed by
// hue, saturation and brightness. The notes show the Psych interface's skin
// when it is on.
class NoteColorsState : public State {
public:
    void enter() override {
        const Settings& s = app().settings;
        for (int d = 0; d < 4; ++d)
            for (int k = 0; k < 3; ++k) hsv_[d][k] = toHsv(s.noteColors[d][k]);
    }
    void update(double) override {
        App& a = app();
        Input& in = a.input;
        const bool shift = in.isDown(KEY_LEFT_SHIFT) || in.isDown(KEY_RIGHT_SHIFT);
        if (in.back() && !in.pressed(KEY_BACKSPACE)) {
            a.saveSettings();
            sfxPlay(Sfx::Cancel);
            a.change(makeOptions());
            return;
        }
        if (in.pressed(KEY_Q) || in.pressed(KEY_PAGE_UP)) { dir_ = (dir_ + 3) % 4; sfxPlay(Sfx::Scroll); }
        if (in.pressed(KEY_E) || in.pressed(KEY_PAGE_DOWN)) { dir_ = (dir_ + 1) % 4; sfxPlay(Sfx::Scroll); }
        if (in.up()) { row_ = (row_ + 8) % 9; sfxPlay(Sfx::Scroll); }
        if (in.down()) { row_ = (row_ + 1) % 9; sfxPlay(Sfx::Scroll); }
        const int step = in.left() ? -1 : in.right() ? 1 : 0;
        if (step) {
            float& v = hsv_[dir_][row_ / 3][row_ % 3];
            v = row_ % 3 == 0 ? std::fmod(v + step * (shift ? 30.0f : 3.0f) + 360.0f, 360.0f)
                              : clampv(v + step * (shift ? 0.1f : 0.02f), 0.0f, 1.0f);
            apply();
        }
        if (in.pressed(KEY_R)) {  // this direction back to Psych's colours
            const Settings d;
            for (int k = 0; k < 3; ++k) hsv_[dir_][k] = toHsv(d.noteColors[dir_][k]);
            apply();
            sfxPlay(Sfx::Confirm);
        }
        // The mouse: a click on a note picks it; dragging a bar sets it.
        const Vector2 m = gfx::toCanvas(GetMousePosition());
        if (in.mousePressed(MOUSE_BUTTON_LEFT))
            for (int d = 0; d < 4; ++d)
                if (CheckCollisionPointRec(m, noteBox(d))) dir_ = d;
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            for (int r = 0; r < 9; ++r) {
                const Rectangle b = bar(r);
                if (!CheckCollisionPointRec(m, {b.x - 4, b.y - 6, b.width + 8, b.height + 12})) continue;
                row_ = r;
                const float f = saturate((m.x - b.x) / b.width);
                hsv_[dir_][r / 3][r % 3] = r % 3 == 0 ? f * 359.9f : f;
                apply();
            }
    }
    void draw() override {
        App& a = app();
        const PsychUi& ui = psychUi();
        const bool psych = a.psychMenusActive();
        gfx::beginCanvas();
        if (psych) {
            drawPsychMenuBackground(1, Color{234, 113, 253, 255}, 0, false, a.now, 0xEA71FD, menuPulse());
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 150});
            if (ui.alphabet("NOTE COLORS", 56.0f, 16.0f, 52.0f, 1.0f) < 0)
                ui.uiText("NOTE COLORS", 56.0f, 18.0f, 44.0f, WHITE, -1, 2.5f);
        } else {
            drawMenuBackground(a.now, 0x5C9CFF, menuPulse());
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.35f));
            gfx::text(FontId::Title, "NOTE COLORS", 60, 22, 48, kGold, Align::Left, 2.0f);
        }
        static const char* dirs[4] = {"LEFT", "DOWN", "UP", "RIGHT"};
        for (int d = 0; d < 4; ++d) {
            const Rectangle b = noteBox(d);
            const Vector2 c{b.x + b.width * 0.5f, b.y + b.height * 0.5f};
            if (d == dir_) DrawRectangleRoundedLinesEx(b, 0.15f, 8, 3, kGold);
            ui.beginDir(d);
            const bool drawn = ui.note(d, c, 120, 1, 0);
            ui.endDir();
            if (!drawn) {  // no Psych skin: the three colours as rings
                DrawCircleV(c, 52, rgb(a.settings.noteColors[d][2]));
                DrawCircleV(c, 44, rgb(a.settings.noteColors[d][0]));
                DrawCircleV({c.x - 14, c.y - 14}, 10, rgb(a.settings.noteColors[d][1]));
            }
            if (psych) ui.uiText(dirs[d], c.x, b.y + b.height + 6, 20.0f, d == dir_ ? kGold : WHITE, 0, 1.5f);
            else gfx::text(FontId::Ui, dirs[d], c.x, b.y + b.height + 6, 20, d == dir_ ? kGold : kIvory, Align::Center, 1.5f);
        }
        static const char* channels[3] = {"Main", "Highlight", "Outline"};
        static const char* parts[3] = {"Hue", "Saturation", "Brightness"};
        for (int r = 0; r < 9; ++r) {
            const Rectangle b = bar(r);
            const int ch = r / 3, part = r % 3;
            if (part == 0) {
                const uint32_t c = a.settings.noteColors[dir_][ch];
                if (psych) ui.uiText(channels[ch], 120.0f, b.y - 4.0f, 24.0f, WHITE, -1, 1.5f);
                else gfx::text(FontId::Ui, channels[ch], 120, b.y - 4, 24, kIvory, Align::Left, 1.5f);
                DrawRectangleRounded({120, b.y + 26, 120, 40}, 0.3f, 6, rgb(c));
                if (psych) ui.uiText(TextFormat("#%06X", unsigned(c)), 180.0f, b.y + 72.0f, 16.0f, WHITE, 0, 1.25f);
                else gfx::text(FontId::Ui, TextFormat("#%06X", unsigned(c)), 180, b.y + 72, 16, kIvory, Align::Center, 1.0f);
            }
            // The bar shows what each value would give.
            const Vector3 h = {hsv_[dir_][ch][0], hsv_[dir_][ch][1], hsv_[dir_][ch][2]};
            for (int i = 0; i < 48; ++i) {
                const float f = (i + 0.5f) / 48.0f;
                Vector3 q = h;
                if (part == 0) q.x = f * 360.0f;
                else if (part == 1) q.y = f;
                else q.z = f;
                DrawRectangleRec({b.x + b.width * i / 48.0f, b.y, b.width / 48.0f + 1, b.height}, ColorFromHSV(q.x, q.y, q.z));
            }
            const float v = part == 0 ? h.x / 360.0f : part == 1 ? h.y : h.z;
            DrawRectangleLinesEx(b, r == row_ ? 2.5f : 1.0f, r == row_ ? kGold : fade(kIvory, 0.5f));
            DrawRectangleRec({b.x + b.width * v - 3, b.y - 5, 6, b.height + 10}, WHITE);
            if (psych) ui.uiText(parts[part], b.x - 14.0f, b.y - 1.0f, 18.0f, r == row_ ? kGold : fade(WHITE, 0.85f), 1, 1.25f);
            else gfx::text(FontId::Ui, parts[part], b.x - 14, b.y - 1, 18, r == row_ ? kGold : fade(kIvory, 0.8f), Align::Right, 1.0f);
        }
        if (!a.touchMode) {
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 165});
            const char* ch = "Q/E or click: note   UP/DOWN: bar   LEFT/RIGHT change (SHIFT faster)   R reset   ESC back";
            if (psych) ui.uiText(ch, gfx::W * 0.5f, gfx::H - 31.0f, 18.0f, Color{220, 214, 235, 255}, 0, 1.25f);
            else gfx::text(FontId::Ui, ch, gfx::W / 2, gfx::H - 33, 20, Color{220, 214, 235, 255}, Align::Center);
        }
        gfx::endCanvas();
    }

private:
    std::array<float, 3> hsv_[4][3] = {};
    int dir_ = 0, row_ = 0;

    static Color rgb(uint32_t c) { return Color{(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c, 255}; }
    static std::array<float, 3> toHsv(uint32_t c) {
        const Vector3 v = ColorToHSV(rgb(c));
        return {v.x, v.y, v.z};
    }
    Rectangle noteBox(int d) const { return {300.0f + d * 180.0f, 90, 150, 150}; }
    Rectangle bar(int r) const { return {470, 300.0f + (r / 3) * 120.0f + (r % 3) * 34.0f, 640, 22}; }
    void apply() {
        Settings& s = app().settings;
        for (int k = 0; k < 3; ++k) {
            const float* q = hsv_[dir_][k].data();
            const Color c = ColorFromHSV(q[0], q[1], q[2]);
            s.noteColors[dir_][k] = uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | c.b;
        }
        psychUi().setPalette(s.noteColors);
    }
};

// -------------------------------------------------------------- note skin --

// Psych's Note Skins option with the notes on show: the skins on the left
// (Default and the noteSkins/ of the Psych copy and of every installed mod),
// the chosen one on the right as a song shows it: receptors, notes coming up,
// holds and the receptors' hit animation. UP/DOWN (or a click on a name)
// change it at once; ENTER or ESC save and go back.
class NoteSkinState : public State {
public:
    void enter() override {
        start_ = app().now;
        listSkins();
        anim_ = float(sel_);
    }
    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        anim_ = approach(anim_, float(sel_), 14.0f, float(dt));
        if ((in.back() && !in.pressed(KEY_BACKSPACE)) || in.accept()) {
            if (pending_) apply();
            a.saveSettings();
            sfxPlay(in.accept() ? Sfx::Confirm : Sfx::Cancel);
            a.change(makeOptions());
            return;
        }
        const int n = rows();
        int to = sel_;
        if (in.up()) to = (sel_ + n - 1) % n;
        if (in.down()) to = (sel_ + 1) % n;
        if (in.pressed(KEY_PAGE_UP)) to = std::max(0, sel_ - 8);
        if (in.pressed(KEY_PAGE_DOWN)) to = std::min(n - 1, sel_ + 8);
        if (in.pressed(KEY_HOME)) to = 0;
        if (in.pressed(KEY_END)) to = n - 1;
        const float wheel = in.wheel();
        if (wheel > 0.5f) to = std::max(0, sel_ - 1);
        if (wheel < -0.5f) to = std::min(n - 1, sel_ + 1);
        // A click (or a tap) on a name picks it; not the touch buttons.
        if (in.mousePressed(MOUSE_BUTTON_LEFT) && !(a.touchMode && a.touch.covers(GetMousePosition()))) {
            const Vector2 m = gfx::toCanvas(GetMousePosition());
            for (int i = 0; i < n; ++i)
                if (CheckCollisionPointRec(m, rowBox(i))) to = i;
        }
        if (to != sel_) {
            sel_ = to;
            a.settings.noteSkin = sel_ == 0 ? std::string() : skins_[size_t(sel_ - 1)];
            // Loaded when the choice rests a moment: holding DOWN through a
            // long list does not load every skin on the way.
            pending_ = true;
            pendingAt_ = a.now + 0.12;
            sfxPlay(Sfx::Scroll);
        }
        if (pending_ && a.now >= pendingAt_) apply();
    }
    void draw() override {
        App& a = app();
        const PsychUi& ui = psychUi();
        const bool psych = a.psychMenusActive();
        gfx::beginCanvas();
        if (psych) {
            drawPsychMenuBackground(1, Color{234, 113, 253, 255}, 0, false, a.now, 0xEA71FD, menuPulse());
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 145});
            if (ui.alphabet("NOTE SKIN", 56.0f, 16.0f, 52.0f, 1.0f) < 0)
                ui.uiText("NOTE SKIN", 56.0f, 18.0f, 44.0f, WHITE, -1, 2.5f);
            ui.uiText(TextFormat("%d to choose from", rows()), 64.0f, 82.0f, 18.0f, fade(WHITE, 0.85f), -1, 1.25f);
        } else {
            drawMenuBackground(a.now, 0x5C9CFF, menuPulse());
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.3f));
            gfx::text(FontId::Title, "NOTE SKIN", 60, 22, 48, kGold, Align::Left, 2.0f);
            gfx::text(FontId::Ui, TextFormat("%d to choose from", rows()), 64, 82, 22, fade(kIvory, 0.8f), Align::Left, 1.0f);
        }

        // The names.
        const float top = listTop(), bottom = listBottom(), mid = (top + bottom) * 0.5f - 22;
        for (int i = 0; i < rows(); ++i) {
            const Rectangle b = rowBox(i);
            if (b.y < top - 4 || b.y + b.height > bottom + 4) continue;
            const bool on = i == sel_;
            const float alpha = saturate(1.0f - std::fabs(b.y - mid) / (bottom - top) * 1.2f) * 0.7f + 0.3f;
            if (on) DrawRectangleRounded(b, 0.3f, 8, Color{255, 214, 120, 46});
            const std::string name = i == 0 ? std::string("Default") : skins_[size_t(i - 1)];
            if (psych) {
                const std::string label = on ? ("> " + name) : name;
                if (ui.alphabet(label, b.x + 14.0f, b.y + 2.0f, 38.0f, alpha, on) < 0)
                    ui.uiText(label, b.x + 16.0f, b.y + 8.0f, 28.0f, fade(on ? kGold : WHITE, alpha), -1, 2.0f);
            } else {
                gfx::text(FontId::Ui, on ? "> " + name : name, b.x + 16, b.y + 6, 30, fade(on ? kGold : kIvory, alpha), Align::Left, 1.5f);
            }
        }

        // The preview: the skin as a song shows it.
        const Rectangle p = panel();
        DrawRectangleRounded(p, 0.06f, 8, Color{8, 14, 34, 170});
        DrawRectangleRoundedLinesEx(p, 0.06f, 8, 2, fade(kGold, 0.5f));
        const bool skinOn = ui.active();
        const float size = 104, gap = 120;
        const float strumY = p.y + 80, fadeY = p.y + p.height - 20;
        const float speed = 0.45f;  // pixels per millisecond (Psych's scroll speed 1)
        const double ms = (a.now - start_) * 1000.0;
        const double loop = kLoopMs;
        const float alphaAll = skinOn ? 1.0f : 0.45f;
        const Vector2 o = gfx::canvasOrigin();
        const float cs = gfx::canvasScale();
        BeginScissorMode(int(o.x + p.x * cs), int(o.y + p.y * cs), int(p.width * cs), int(p.height * cs));
        for (int dir = 0; dir < 4; ++dir) {
            const Vector2 c{p.x + p.width * 0.5f + (float(dir) - 1.5f) * gap, strumY};
            // The receptor lights up while a note of its lane is being hit.
            float conf = 0;
            double ageSec = 0.0;
            for (const PreviewNote& n : kPattern) {
                if (n.lane != dir) continue;
                const double at = std::fmod(ms - n.t + loop * 4, loop);  // time since this note's last hit
                const double lit = std::max(n.len, 156.0);
                if (at < lit) {
                    const float cVal = float(n.len > 0 && at < n.len ? 1.0 : 1.0 - (at - std::max(0.0, n.len)) / 156.0);
                    if (cVal > conf) {
                        conf = cVal;
                        ageSec = at / 1000.0;
                    }
                }
            }
            const int state = conf > 0.01f ? 2 : 0;
            if (state) ui.beginDir(dir);  // idle receptors stay grey
            const bool drawn = ui.strum(dir, c, size, state, alphaAll, 0, ageSec);
            if (state) ui.endDir();
            if (!drawn) {
                if (conf > 0.01f) {
                    gfx::glow(c, size * 0.95f * conf, fade(gfx::dirColor(dir), 0.6f * alphaAll));
                    gfx::drawNote(dir, c, size, alphaAll, 1.0f + 0.06f * conf);
                } else {
                    gfx::drawReceptor(dir, c, size, fade(WHITE, alphaAll));
                }
            }
            // The notes coming up, and their holds, under the pattern's loop.
            ui.beginDir(dir);
            const double cycle = std::floor(ms / loop);
            for (int k = 0; k < 3; ++k)
                for (const PreviewNote& n : kPattern) {
                    if (n.lane != dir) continue;
                    const double t = n.t + (cycle + k) * loop;
                    if (n.len > 0 && ms < t + n.len) {
                        const float y0 = strumY + float(std::max(0.0, t - ms)) * speed;
                        const float y1 = strumY + float(t + n.len - ms) * speed;
                        const float al = alphaAll * saturate((fadeY - y0) / 80.0f);
                        if (y1 > y0 + 1 && al > 0.01f && !ui.hold(dir, c.x, y0, y1, size * 0.3f, al))
                            gfx::drawSustain(dir, c.x, y0, y1, size * 0.3f, al * 0.85f + 0.15f, true);
                    }
                    if (t < ms) continue;
                    const float y = strumY + float(t - ms) * speed;
                    const float al = alphaAll * saturate((fadeY - y) / 80.0f);
                    if (al <= 0.01f) continue;
                    if (!ui.note(dir, {c.x, y}, size, al, 0)) gfx::drawNote(dir, {c.x, y}, size, al);
                }
            ui.endDir();
        }
        EndScissorMode();
        // What the preview shows.
        std::string about, why;
        if (!skinOn) {
            about = "No Psych Engine interface to show";
            why = a.psychReport.empty() ? std::string("Bundled Psych Engine assets were not found.") : a.psychReport;
        }
        if (!about.empty()) {
            const Rectangle box{p.x + 30, p.y + p.height * 0.5f - 70, p.width - 60, 140};
            DrawRectangleRounded(box, 0.2f, 8, Color{0, 0, 0, 200});
            if (psych) ui.uiText(about, box.x + box.width * 0.5f, box.y + 18, 28.0f, kGold, 0, 1.5f);
            else gfx::text(FontId::Ui, about, box.x + box.width * 0.5f, box.y + 18, 30, kGold, Align::Center, 1.5f);
            float y = box.y + 62;
            for (const std::string& line : wrapWidth(ui, psych, FontId::Ui, why, 20.0f, box.width - 40)) {
                if (psych) ui.uiText(line, box.x + box.width * 0.5f, y, 20.0f, WHITE, 0, 1.25f);
                else gfx::text(FontId::Ui, line, box.x + box.width * 0.5f, y, 22, kIvory, Align::Center);
                y += 26;
            }
        }

        if (!a.touchMode) {
            DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 165});
            const char* sh = "UP/DOWN or click: skin   ENTER/ESC save and back";
            if (psych) ui.uiText(sh, gfx::W * 0.5f, gfx::H - 31.0f, 18.0f, Color{220, 214, 235, 255}, 0, 1.25f);
            else gfx::text(FontId::Ui, sh, gfx::W / 2, gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
        }
        gfx::endCanvas();
    }

private:
    // The preview's chart: two bars at 120 BPM, looped (times and lengths in ms).
    struct PreviewNote {
        double t;
        int lane;
        double len;
    };
    static constexpr double kLoopMs = 4000;
    static constexpr PreviewNote kPattern[] = {
        {0, 0, 0},    {250, 1, 0},  {500, 2, 0},    {750, 3, 0},  {1000, 1, 750}, {1250, 3, 0},
        {1500, 0, 0}, {1750, 2, 0}, {2000, 3, 0},   {2250, 0, 0}, {2500, 2, 500}, {2750, 1, 0},
        {3000, 3, 0}, {3250, 0, 0}, {3250, 3, 0},   {3500, 1, 0}, {3750, 2, 0},
    };

    std::vector<std::string> skins_;  // row k + 1; row 0 is Default
    int sel_ = 0;
    float anim_ = 0;
    double start_ = 0;
    bool pending_ = false;
    double pendingAt_ = 0;

    int rows() const { return int(skins_.size()) + 1; }
    // Default and the skins of the Psych copy in use and of every installed mod.
    void listSkins() {
        const App& a = app();
        skins_ = psychNoteSkins(a.psychFolderInUse, a.library.allAssetRoots());
        sel_ = 0;
        for (size_t k = 0; k < skins_.size(); ++k)
            if (skins_[k] == a.settings.noteSkin) sel_ = int(k) + 1;
    }
    void apply() {
        pending_ = false;
        App& a = app();
        PsychUi& ui = psychUi();
        if (ui.active()) ui.changeNoteSkin(a.settings.noteSkin);
        else ui.setNoteSkin(a.settings.noteSkin);
        a.saveSettings();  // kept even if the game is closed on this screen
    }
    // Touchscreens: the arrows and OK/BACK buttons cover the bottom corners.
    float listTop() const { return 120; }
    float listBottom() const { return app().touchMode ? 390.0f : 640.0f; }
    Rectangle rowBox(int i) const {
        const float mid = (listTop() + listBottom()) * 0.5f - 22;
        return {50, mid + (float(i) - anim_) * 48, 470, 44};
    }
    Rectangle panel() const { return {560, 100, 660, app().touchMode ? 420.0f : 540.0f}; }
};

std::unique_ptr<State> makeNoteSkins() { return std::make_unique<NoteSkinState>(); }
std::unique_ptr<State> makeNoteColors() { return std::make_unique<NoteColorsState>(); }
std::unique_ptr<State> makeKeyBindings() { return std::make_unique<KeyBindState>(); }

std::unique_ptr<State> makeOptions() { return std::make_unique<OptionsState>(); }
std::unique_ptr<State> makeCalibration() { return std::make_unique<CalibrationState>(); }

}  // namespace gh
