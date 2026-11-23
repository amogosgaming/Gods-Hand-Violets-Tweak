// Astralis Engine. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// Title, main menu, story, freeplay and credits screens.
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "basegame.hpp"
#include "characters.hpp"
#include "common.hpp"
#include "editor_ui.hpp"
#include "gfx.hpp"
#include "platform.hpp"
#include "psych.hpp"
#include "psych_ui.hpp"
#include "raylib.h"
#include "scene.hpp"

#ifndef GH_VERSION
#define GH_VERSION "dev"  // set by CMake from project(VERSION)
#endif

namespace gh {

using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {

const Color kGold{255, 222, 64, 255};   // the accent: a bright, cartoon yellow
const Color kIvory{242, 244, 255, 255};  // text: a cool white

Color diffColor(const std::string& d) {
    const std::string l = toLower(d);
    if (l == "easy") return Color{120, 220, 140, 255};
    if (l == "normal") return Color{255, 214, 100, 255};
    if (l == "hard") return Color{255, 120, 90, 255};
    if (l == "infernal") return Color{255, 50, 70, 255};
    return Color{190, 160, 255, 255};
}

void hints(const char* s) {
    if (app().touchMode) return;  // the on-screen buttons speak for themselves
    DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
    if (app().psychMenusActive())
        psychUi().uiText(s, gfx::W * 0.5f, gfx::H - 31.0f, 18.0f, Color{220, 214, 235, 255}, 0, 1.25f);
    else
        gfx::text(FontId::Ui, s, gfx::W / 2, gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
}

std::vector<std::string> wrap(const std::string& s, size_t width) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush = [&] {
        if (cur.size() + word.size() + (cur.empty() ? 0 : 1) > width && !cur.empty()) {
            lines.push_back(cur);
            cur.clear();
        }
        if (!word.empty()) cur += (cur.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flush();
        else word += c;
    }
    flush();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Background jobs that must not block the UI (or the exit) while composing.
std::atomic<int> g_jobs{0};
struct PcmJob {
    struct Slot {
        std::mutex m;
        bool ready = false;
        PcmPtr pcm;
    };
    std::shared_ptr<Slot> slot;
    template <class F> void start(F fn) {
        slot = std::make_shared<Slot>();
        auto s = slot;
        ++g_jobs;
        std::thread([s, fn] {
            PcmPtr r = fn();
            {
                std::lock_guard<std::mutex> lk(s->m);
                s->pcm = r;
                s->ready = true;
            }
            --g_jobs;
        }).detach();
    }
    bool running() const { return slot != nullptr; }
    bool ready() const {
        if (!slot) return false;
        std::lock_guard<std::mutex> lk(slot->m);
        return slot->ready;
    }
    PcmPtr take() {
        PcmPtr r;
        if (slot) {
            std::lock_guard<std::mutex> lk(slot->m);
            r = slot->pcm;
        }
        slot.reset();
        return r;
    }
};

}  // namespace

void waitForBackgroundJobs() {
    for (int i = 0; i < 600 && g_jobs.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// ------------------------------------------------------------------ Title --

static bool gTitleIntroSeen = false;

class TitleState : public State {
public:
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        t.tapCode = KEY_ENTER;
        return t;
    }
    void enter() override {
        const App& a = app();
        startAt_ = a.now;
        beatAt_ = a.now;
        lastBeat_ = -1;
        danceRight_ = false;
        leaving_ = false;
        introDone_ = gTitleIntroSeen || !a.psychMenusActive();
        flash_ = 0.0f;
        const auto& intros = psychUi().introTexts();
        if (!intros.empty()) {
            const size_t idx = size_t(std::llabs(static_cast<long long>(a.now * 1000.0)) + 17) % intros.size();
            wacky_ = intros[idx];
        } else {
            wacky_ = {"ASTRALIS ENGINE", "NATIVE C++20"};
        }
    }
    void skipIntro() {
        if (introDone_) return;
        introDone_ = true;
        gTitleIntroSeen = true;
        flash_ = 1.0f;
        beatAt_ = app().now;
    }
    void update(double) override {
        App& a = app();
        flash_ = std::max(0.0f, flash_ - float(a.dt) * 1.4f);
        const double bpm = std::max(60.0, psychUi().titleBpm());
        const int curBeat = int(std::floor(std::max(0.0, a.now - startAt_) * (bpm / 60.0)));
        if (curBeat != lastBeat_) {
            lastBeat_ = curBeat;
            beatAt_ = a.now;
            danceRight_ = !danceRight_;
            if (!introDone_ && curBeat >= 16) skipIntro();
        }
        const bool hit = (a.input.accept() || a.input.firstPressed() != 0) && !a.input.pressed(KEY_F11);
        if (!introDone_) {
            if (hit) skipIntro();
            return;
        }
        if (!leaving_ && hit) {
            leaving_ = true;
            leaveAt_ = a.now;
            flash_ = 1.0f;
            sfxPlay(Sfx::Confirm);
        }
        if (leaving_ && a.now - leaveAt_ > 1.0) a.change(makeMainMenu());
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        const float pulse = menuPulse();
        gfx::beginCanvas();
        if (a.psychMenusActive()) {
            const PsychUi& ui = psychUi();
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, BLACK);
            if (!introDone_) {
                auto drawLines = [&](std::initializer_list<std::string> lines, float yOff = 0.0f) {
                    int idx = 0;
                    for (const std::string& l : lines) {
                        const float y = 200.0f + float(idx) * 68.0f + yOff;
                        if (ui.alphabetCentered(l, gfx::W * 0.5f, y, 66.0f, 1.0f) < 0)
                            ui.uiText(l, gfx::W * 0.5f, y, 56.0f, WHITE, 0, 2.5f);
                        ++idx;
                    }
                };
                const int b = std::max(0, lastBeat_);
                if (b >= 1 && b < 4) {
                    if (b == 1) drawLines({"PSYCH ENGINE BY", "SHADOW MARIO"});
                    else if (b == 2) drawLines({"PSYCH ENGINE BY", "SHADOW MARIO", "RIVEREN"});
                    else drawLines({"PSYCH ENGINE BY", "SHADOW MARIO", "RIVEREN", "ASTRALIS PORT"});
                } else if (b >= 5 && b < 8) {
                    if (b == 5) drawLines({"NOT ASSOCIATED", "WITH"}, -40.0f);
                    else if (b == 6) drawLines({"NOT ASSOCIATED", "WITH", "NEWGROUNDS"}, -40.0f);
                    else {
                        drawLines({"NOT ASSOCIATED", "WITH", "NEWGROUNDS"}, -40.0f);
                        ui.drawNewgroundsLogo({gfx::W * 0.5f, 510.0f}, 0.8f, 1.0f);
                    }
                } else if (b >= 9 && b < 12) {
                    if (b == 9) drawLines({wacky_.first});
                    else drawLines({wacky_.first, wacky_.second});
                } else if (b >= 13) {
                    if (b == 13) drawLines({"FRIDAY"});
                    else if (b == 14) drawLines({"FRIDAY", "NIGHT"});
                    else drawLines({"FRIDAY", "NIGHT", "FUNKIN"});
                }
                gfx::endCanvas();
                return;
            }
            const double beatElapsed = std::max(0.0, now - beatAt_);
            const bool drewGf = ui.drawTitleGf(danceRight_, beatElapsed, 1.0f);
            const bool drewLogo = ui.drawTitleLogo(beatElapsed, 1.0f);
            const bool drewEnter = ui.drawTitleEnter(leaving_, a.settings.flashing, now, leaveAt_, 1.0f);
            if (!drewGf && !drewLogo) {
                const float s = 1.0f + 0.045f * pulse;
                if (ui.alphabetCentered("ASTRALIS", gfx::W * 0.5f, 185.0f - 10.0f * pulse, 92.0f * s, 1.0f) < 0)
                    ui.uiText("ASTRALIS", gfx::W * 0.5f, 175.0f - 8.0f * pulse, 92.0f * s, WHITE, 0, 3.0f);
            }
            if (!drewEnter) {
                const float blink = leaving_ ? (std::fmod(float(now) * 14.0f, 2.0f) < 1.0f ? 1.0f : 0.15f)
                                             : 0.45f + 0.55f * (0.5f + 0.5f * std::sin(float(now) * 3.5f));
                const char* prompt = a.touchMode ? "TAP TO BEGIN" : "PRESS ENTER TO BEGIN";
                const Color pc = leaving_ ? Color{51, 255, 255, 255} : Color{51, 51, 204, 255};
                ui.uiText(prompt, gfx::W * 0.5f, 576.0f, 38.0f, fade(leaving_ ? pc : gfx::mix(pc, WHITE, blink), blink), 0, 2.5f);
            }
            if (!a.mixer.audioOk())
                ui.uiText("No audio device found - the game will run silently", gfx::W * 0.5f, 672.0f, 20.0f,
                          Color{255, 160, 140, 255}, 0, 1.5f);
            if (flash_ > 0 && a.settings.flashing) DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(WHITE, flash_ * 0.85f));
            gfx::endCanvas();
            return;
        }
        drawMenuBackground(now, 0xF4C76B, pulse);
        const float s = 1.0f + 0.025f * pulse;
        gfx::glowText(FontId::Title, "ASTRALIS", gfx::W / 2, 170 - 6 * s, 108 * s, kGold, Color{255, 190, 80, 255});
        gfx::text(FontId::Title, "Astralis Engine", gfx::W / 2, 296, 38, kIvory, Align::Center, 2.0f);
        const float blink = leaving_ ? (std::fmod(float(now) * 12.0f, 2.0f) < 1.0f ? 1.0f : 0.2f)
                                     : 0.55f + 0.45f * std::sin(float(now) * 3.5f);
        gfx::text(FontId::Ui, a.touchMode ? "TAP TO BEGIN" : "PRESS ENTER TO BEGIN", gfx::W / 2, 470, 40, fade(WHITE, blink),
                  Align::Center, 2.0f);
        gfx::text(FontId::Ui, "Astralis Engine  -  v" GH_VERSION, gfx::W / 2, 680, 22, fade(kIvory, 0.7f), Align::Center, 1.0f);
        if (!a.mixer.audioOk())
            gfx::text(FontId::Ui, "No audio device found - the game will run silently", gfx::W / 2, 520, 22,
                      Color{255, 160, 140, 255}, Align::Center, 1.5f);
        if (flash_ > 0) DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(WHITE, flash_ * 0.8f));
        gfx::endCanvas();
    }

private:
    bool introDone_ = false;
    bool leaving_ = false;
    bool danceRight_ = false;
    int lastBeat_ = -1;
    double startAt_ = 0;
    double beatAt_ = 0;
    double leaveAt_ = 0;
    float flash_ = 0;
    std::pair<std::string, std::string> wacky_;
};

// -------------------------------------------------------------- Main menu --

constexpr const char* kMainItems[] = {"STORY MODE", "FREEPLAY", "CREDITS", "OPTIONS", "QUIT"};
constexpr int kMainCount = int(sizeof(kMainItems) / sizeof(kMainItems[0]));

class MainMenuState : public State {
public:
    void enter() override {
        app().mainSel = clampv(app().mainSel, 0, kMainCount - 1);
        anim_ = float(app().mainSel);
    }
    void update(double dt) override {
        App& a = app();
        int& sel = a.mainSel;
        anim_ = approach(anim_, float(sel), 14.0f, float(dt));
        if (chosen_ >= 0) {
            if (a.now - chosenAt_ > 0.55) {
                switch (chosen_) {
                    case 0: a.change(makeStory()); break;
                    case 1:
                        a.songListMode = App::SongListMode::Play;
                        a.change(makeFreeplay());
                        break;
                    case 2: a.change(makeCredits()); break;
                    case 3: a.change(makeOptions()); break;
                    default: a.quit = true; break;
                }
            }
            return;
        }
        if (a.input.up()) { sel = (sel + kMainCount - 1) % kMainCount; sfxPlay(Sfx::Scroll); }
        if (a.input.down()) { sel = (sel + 1) % kMainCount; sfxPlay(Sfx::Scroll); }
        if (a.input.accept()) {
            chosen_ = sel;
            chosenAt_ = a.now;
            sfxPlay(sel == kMainCount - 1 ? Sfx::Cancel : Sfx::Confirm);
        }
        if (a.input.back()) { sfxPlay(Sfx::Cancel); a.change(makeTitle()); }
        // 7: the editors (chart, character, stage, events, modchart, SNIFF), as Psych's Master Editor Menu.
        if (a.input.pressed(KEY_SEVEN) || a.input.pressed(KEY_KP_7)) {
            sfxPlay(Sfx::Confirm);
            a.change(makeEditorMenu());
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        const float pulse = menuPulse();
        const char* const* items = kMainItems;
        gfx::beginCanvas();
        if (a.psychMenusActive()) {
            const PsychUi& ui = psychUi();
            const bool magenta = chosen_ >= 0 && a.settings.flashing && std::fmod(float(now - chosenAt_) * 10.0f, 2.0f) < 1.0f;
            drawPsychMenuBackground(magenta ? 3 : 0, WHITE, anim_ * 42.0f, true, now, 0xFDE871, pulse);
            for (int i = 0; i < kMainCount; ++i) {
                const bool on = i == a.mainSel;
                const float d = float(i) - anim_;
                const float y = 330.0f + d * 130.0f;
                if (y < -120.0f || y > gfx::H + 120.0f) continue;
                float itemAlpha = on ? 1.0f : 0.78f;
                if (chosen_ >= 0) {
                    if (on) itemAlpha = std::fmod(float(now - chosenAt_) * 14.0f, 2.0f) < 1.0f ? 1.0f : 0.15f;
                    else itemAlpha *= std::max(0.0f, 1.0f - float(now - chosenAt_) * 3.0f);
                }
                if (itemAlpha <= 0.01f) continue;
                int spriteIdx = -1;
                if (i == 0) spriteIdx = 0;       // story_mode
                else if (i == 1) spriteIdx = 1;  // freeplay
                else if (i == 2) spriteIdx = 3;  // credits
                else if (i == 3) spriteIdx = 2;  // options
                bool drawn = false;
                if (spriteIdx >= 0) drawn = ui.drawMainMenuItem(spriteIdx, {gfx::W * 0.5f, y}, on, now, itemAlpha);
                if (!drawn) {
                    const float h = on ? 72.0f : 56.0f;
                    if (ui.alphabetCentered(items[i], gfx::W * 0.5f, y - h * 0.5f, h, itemAlpha) < 0)
                        ui.uiText(items[i], gfx::W * 0.5f, y - h * 0.5f, on ? 56.0f : 44.0f,
                                  fade(on ? kGold : WHITE, itemAlpha), 0, 2.5f);
                }
            }
            ui.uiText("Astralis Engine v" GH_VERSION, 12.0f, gfx::H - 88.0f, 18.0f, WHITE, -1, 1.5f);
            ui.uiText(a.library.report(), 12.0f, gfx::H - 66.0f, 16.0f, fade(WHITE, 0.85f), -1, 1.25f);
            hints("UP/DOWN select   ENTER confirm   7 editors   ESC back   F11 fullscreen");
            gfx::endCanvas();
            return;
        }
        drawMenuBackground(now, 0x8C6CFF, pulse, true);
        gfx::text(FontId::Title, "ASTRALIS", 60, 40, 44, kGold, Align::Left, 2.0f);
        for (int i = 0; i < kMainCount; ++i) {
            const bool on = i == a.mainSel;
            const float y = 160.0f + i * 84.0f - (on ? 6.0f : 0.0f);
            float size = on ? 56.0f + 4.0f * pulse : 42.0f;
            Color c = on ? kGold : fade(kIvory, 0.55f);
            if (chosen_ >= 0 && on && std::fmod(float(now - chosenAt_) * 10.0f, 2.0f) < 1.0f) c = WHITE;
            if (chosen_ >= 0 && !on) c = fade(c, std::max(0.0f, 1.0f - float(now - chosenAt_) * 3.0f));
            if (on) gfx::glowText(FontId::Title, items[i], gfx::W / 2, y, size, c, Color{255, 180, 80, 255});
            else gfx::text(FontId::Title, items[i], gfx::W / 2, y + 6, size, c, Align::Center, 2.0f);
            if (on) {
                const float w = gfx::measure(FontId::Title, items[i], size).x * 0.5f + 40 + 6 * pulse;
                gfx::drawNote(gfx::kRight, {gfx::W / 2 - w, y + size * 0.55f}, 44);
                gfx::drawNote(gfx::kLeft, {gfx::W / 2 + w, y + size * 0.55f}, 44);
            }
        }
        hints("UP/DOWN select   ENTER confirm   7 editors   ESC back   F11 fullscreen");
        gfx::text(FontId::Ui, a.library.report(), 16, gfx::H - 70, 20, fade(kIvory, 0.6f));
        gfx::endCanvas();
    }

private:
    float anim_ = 0;
    int chosen_ = -1;
    double chosenAt_ = 0;
};

// ------------------------------------------------------------------ Story --

class StoryState : public State {
public:
    struct WeekDef {
        const char* id;
        const char* title;
        const char* motto;
        std::vector<const char*> songKeys;
        std::vector<const char*> trackNames;
        Color banner;
        bool hasStageBg;
        bool hasDad;
    };
    static const std::vector<WeekDef>& weeks() {
        static const std::vector<WeekDef> w = {
            {"week:tutorial", "TUTORIAL", "TEACHING TIME", {"tutorial"}, {"Tutorial"}, Color{249, 207, 81, 255}, false, false},
            {"week:week1", "WEEK 1", "DADDY DEAREST", {"bopeebo", "fresh", "dad-battle"}, {"Bopeebo", "Fresh", "Dad Battle"},
             Color{249, 207, 81, 255}, true, true},
        };
        return w;
    }
    static std::string findSongId(const App& a, const std::string& key) {
        const std::string k = toLower(key);
        for (const SongEntry& e : a.library.songs())
            if (e.pack.empty() && toLower(e.songKey) == k) return e.id;
        for (const SongEntry& e : a.library.songs())
            if (toLower(e.songKey) == k) return e.id;
        return "custom:songs/" + k;
    }
    void enter() override {
        beatAt_ = app().now;
        lastBeat_ = -1;
        diffChangedAt_ = -10.0;
        go_ = false;
    }
    void update(double dt) override {
        App& a = app();
        static const char* diffs[3] = {"Easy", "Normal", "Hard"};
        const auto& ws = weeks();
        const int nw = int(ws.size());
        week_ = clampv(week_, 0, nw - 1);
        animWeek_ = approach(animWeek_, float(week_), 12.0f, float(dt));
        int& d = a.storyDiff;
        d = clampv(d, 0, 2);

        const double bpm = std::max(60.0, psychUi().titleBpm());
        const int curBeat = int(std::floor(std::max(0.0, a.now) * (bpm / 60.0)));
        if (curBeat != lastBeat_) {
            lastBeat_ = curBeat;
            beatAt_ = a.now;
        }

        if (go_) {
            if (a.now - confirmAt_ >= 0.85) {
                const WeekDef& w = ws[size_t(week_)];
                std::vector<std::string> ids;
                for (const char* sk : w.songKeys) ids.push_back(findSongId(a, sk));
                if (!ids.empty()) {
                    PlayRequest r;
                    r.songId = ids[0];
                    r.diff = diffs[d];
                    r.story = true;
                    for (size_t i = 1; i < ids.size(); ++i) r.storyQueue.push_back(ids[i]);
                    a.change(makePlay(r));
                }
            }
            return;
        }
        if (a.input.up()) { week_ = (week_ + nw - 1) % nw; sfxPlay(Sfx::Scroll); }
        if (a.input.down()) { week_ = (week_ + 1) % nw; sfxPlay(Sfx::Scroll); }
        if (a.input.left()) { d = (d + 2) % 3; diffChangedAt_ = a.now; sfxPlay(Sfx::Scroll); }
        if (a.input.right()) { d = (d + 1) % 3; diffChangedAt_ = a.now; sfxPlay(Sfx::Scroll); }
        if (a.input.back()) { sfxPlay(Sfx::Cancel); a.change(makeMainMenu()); }
        if (a.input.accept()) {
            const WeekDef& w = ws[size_t(week_)];
            if (w.songKeys.empty()) return;
            go_ = true;
            confirmAt_ = a.now;
            sfxPlay(Sfx::Confirm);
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        static const char* diffs[3] = {"Easy", "Normal", "Hard"};
        const auto& ws = weeks();
        const WeekDef& w = ws[size_t(clampv(week_, 0, int(ws.size()) - 1))];
        const int diffIdx = clampv(a.storyDiff, 0, 2);
        const std::string d = diffs[diffIdx];
        const ScoreEntry* best = a.scores.get(Scores::key(w.id, d));
        gfx::beginCanvas();
        const PsychUi& ui = psychUi();
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, BLACK);

        // Week title sprites drawn before the top banner so scrolling slides cleanly behind it (Psych StoryMenuState.hx).
        for (int i = 0; i < int(ws.size()); ++i) {
            const float wy = 516.0f + (float(i) - animWeek_) * 120.0f;
            if (wy < 360.0f || wy > gfx::H + 80.0f) continue;
            const bool on = i == week_;
            const float alpha = on ? 1.0f : 0.6f;
            const bool flashCyan = go_ && on && a.settings.flashing && std::fmod(float(now - confirmAt_) * 16.0f, 2.0f) < 1.0f;
            const Color tint = flashCyan ? Color{51, 255, 255, 255} : WHITE;
            if (!ui.drawStoryWeekTitle(i, {gfx::W * 0.5f, wy}, alpha, tint)) {
                if (ui.alphabetCentered(ws[size_t(i)].title, gfx::W * 0.5f, wy - 28.0f, on ? 56.0f : 44.0f, alpha, true, tint) < 0)
                    ui.uiText(ws[size_t(i)].title, gfx::W * 0.5f, wy - 22.0f, on ? 48.0f : 38.0f, fade(tint, alpha), 0, 2.5f);
            }
        }

        // Top black bar + yellow Psych Story banner (0..442).
        DrawRectangleRec({0, 0, gfx::W, 56}, BLACK);
        ui.uiText(best ? ("WEEK SCORE: " + fmtThousands(best->score)) : std::string("WEEK SCORE: 0"), 10.0f, 10.0f, 32.0f, WHITE, -1, 0.0f);
        ui.uiText(w.motto, gfx::W - 10.0f, 10.0f, 32.0f, fade(WHITE, 0.7f), 1, 0.0f);
        DrawRectangleRec({0, 56, gfx::W, 386}, w.banner);
        if (w.hasStageBg) ui.drawStoryStageBg({0, 56, gfx::W, 386}, 1.0f);

        // Psych Story Mode MenuCharacters (x = 1280 * 0.25 * (1 + slot) - 150, y = 70).
        const double beatElapsed = std::max(0.0, now - beatAt_);
        const double heyElapsed = go_ ? std::max(0.0, now - confirmAt_) : 0.0;
        bool anyChar = false;
        if (w.hasDad) anyChar |= ui.drawStoryChar(0, {170.0f, 70.0f}, beatElapsed, false, 0.0, 1.0f);
        anyChar |= ui.drawStoryChar(1, {490.0f, 70.0f}, beatElapsed, go_, heyElapsed, 1.0f);
        anyChar |= ui.drawStoryChar(2, {810.0f, 70.0f}, beatElapsed, false, 0.0, 1.0f);
        if (!anyChar) {
            if (ui.alphabetCentered(w.title, gfx::W * 0.5f, 178.0f, 82.0f, 1.0f) < 0)
                ui.uiText(w.title, gfx::W * 0.5f, 178.0f, 76.0f, WHITE, 0, 3.0f);
        }

        // Bottom-left TRACKS header + pink track list (Psych StoryMenuState.hx).
        const Color pink{229, 87, 119, 255};
        if (!ui.drawTracksHeader({gfx::W * 0.07f, 442.0f + 56.0f}, 1.0f))
            ui.uiText("TRACKS", 190.0f, 496.0f, 32.0f, pink, 0, 0.0f);
        for (size_t i = 0; i < w.trackNames.size(); ++i)
            ui.uiText(w.trackNames[i], 190.0f, 550.0f + float(i) * 32.0f, 32.0f, pink, 0, 0.0f);

        // Bottom-right difficulty selector with campaign_menu_UI_assets arrows & menudifficulties/*.png.
        const bool pushLeft = a.input.isDown(KEY_LEFT) || a.input.isDown(KEY_A);
        const bool pushRight = a.input.isDown(KEY_RIGHT) || a.input.isDown(KEY_D);
        const float diffT = saturate(float((now - diffChangedAt_) / 0.07));
        const float diffY = 516.0f - 15.0f * (1.0f - diffT);
        const float diffAlpha = 0.25f + 0.75f * diffT;
        if (!ui.drawCampaignArrow(false, pushLeft, {898.0f, 516.0f}, 1.0f))
            ui.uiText("<", 898.0f, 498.0f, 44.0f, WHITE, 0, 2.0f);
        if (!ui.drawDifficulty(diffIdx, {1066.0f, diffY}, diffAlpha, 1.0f)) {
            const std::string diffUpper = diffIdx == 2 ? "HARD" : (diffIdx == 0 ? "EASY" : "NORMAL");
            ui.uiText(diffUpper, 1066.0f, diffY - 18.0f, 44.0f, fade(diffColor(d), diffAlpha), 0, 2.5f);
        }
        if (!ui.drawCampaignArrow(true, pushRight, {1234.0f, 516.0f}, 1.0f))
            ui.uiText(">", 1234.0f, 498.0f, 44.0f, WHITE, 0, 2.0f);

        hints("UP/DOWN week   LEFT/RIGHT difficulty   ENTER start the week   ESC back");
        gfx::endCanvas();
    }

private:
    int week_ = 0;
    float animWeek_ = 0;
    bool go_ = false;
    int lastBeat_ = -1;
    double beatAt_ = 0;
    double confirmAt_ = 0;
    double diffChangedAt_ = -10.0;
};

// --------------------------------------------------------------- Freeplay --

class FreeplayState : public State {
public:
    ~FreeplayState() override { clearIconCache(); }
    void enter() override {
        rebuild();
        anim_ = float(app().freeplaySel);
        selChangedAt_ = app().now;
        bg_ = currentColor();
    }
    bool wantsMenuMusic() const override { return !previewPlaying_; }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        if (!modsOpen_) t.keys = {{"SEARCH", KEY_F}, {"MODIFIERS", KEY_TAB}, {"MOD PACKS", KEY_M}, {"CHART EDITOR", KEY_SEVEN}, {"RESCAN", KEY_R}};
        if (!modsOpen_ && platform::kAndroid) t.keys.insert(t.keys.begin() + 3, {"IMPORT ZIP", KEY_I});
        return t;
    }
    // While typing a search, the volume keys and the other letters type.
    bool keyReserved(int) const override { return typing_; }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        int& sel = a.freeplaySel;
        const int n = int(list_.size());
        anim_ = approach(anim_, float(sel), 12.0f, float(dt));
        const Color target = currentColor();
        bg_ = gfx::mix(bg_, target, saturate(float(dt) * 4.0f));

        // Searching (JS Engine's Freeplay search): F or / starts typing, the
        // list keeps the songs whose name, mod or id has the text; ENTER keeps
        // the search, ESC clears it. Android: the system's text box.
        if (askingText_) {
            std::string text;
            const platform::TextAsk st = platform::textStatus(&text);
            if (st == platform::TextAsk::Done || st == platform::TextAsk::Cancelled) {
                askingText_ = false;
                platform::resetText();
                if (st == platform::TextAsk::Done) setQuery(text);
            }
            return;
        }
        if (typing_) {
            std::string q = query_;
            for (char32_t c : in.typed()) q += edui::utf8(c);
            if (in.pressed(KEY_BACKSPACE) && !q.empty()) {
                q.pop_back();
                while (!q.empty() && (static_cast<unsigned char>(q.back()) & 0xC0) == 0x80) q.pop_back();
            }
            if (in.pressed(KEY_ESCAPE)) {
                typing_ = false;
                q.clear();
                sfxPlay(Sfx::Cancel);
            } else if (in.pressed(KEY_ENTER) || in.pressed(KEY_KP_ENTER) || in.pressed(KEY_DOWN) || in.pressed(KEY_UP)) {
                typing_ = false;
                sfxPlay(Sfx::Confirm);
            }
            if (q != query_) setQuery(q);
            return;
        }
        if (!modsOpen_ && (in.pressed(KEY_F) || in.pressed(KEY_SLASH))) {
            sfxPlay(Sfx::Scroll);
            if (platform::kAndroid && platform::askText("Search songs", query_)) askingText_ = true;
            else typing_ = true;
            return;
        }
        if (!modsOpen_ && !query_.empty() && in.back()) {  // ESC first clears the search
            setQuery("");
            sfxPlay(Sfx::Cancel);
            return;
        }
        if (modsOpen_) {
            Modifiers& m = a.mods;
            if (in.up()) { modSel_ = (modSel_ + kModRows - 1) % kModRows; sfxPlay(Sfx::Scroll); }
            if (in.down()) { modSel_ = (modSel_ + 1) % kModRows; sfxPlay(Sfx::Scroll); }
            const int dir = in.left() ? -1 : (in.right() ? 1 : 0);
            if (dir || in.accept()) {
                sfxPlay(Sfx::Scroll);
                if (modSel_ == 0) m.rate = clampv(std::round((m.rate + 0.05 * (dir ? dir : 1)) * 20.0) / 20.0, 0.5, 2.0);
                if (modSel_ == 1) { m.botplay = !m.botplay; lastModToggleRow_ = 1; lastModToggleAt_ = a.now; }
                if (modSel_ == 2) { m.practice = !m.practice; lastModToggleRow_ = 2; lastModToggleAt_ = a.now; }
                if (modSel_ == 3) { m.mirror = !m.mirror; lastModToggleRow_ = 3; lastModToggleAt_ = a.now; }
                if (modSel_ == 4) {
                    m.scrollType = (m.scrollType + (dir ? dir : 1) + 3) % 3;
                    m.scrollValue = m.scrollType == 2 ? 2.5 : 1.0;
                }
                if (modSel_ == 5 && m.scrollType != 0) {
                    const double step = m.scrollType == 1 ? 0.05 : 0.1;
                    m.scrollValue = std::round((m.scrollValue + step * (dir ? dir : 1)) / step) * step;
                    m.scrollValue = m.scrollType == 1 ? clampv(m.scrollValue, 0.25, 5.0) : clampv(m.scrollValue, 0.5, 10.0);
                }
                // Kept for the next time the game starts, like the options.
                Settings& s = a.settings;
                s.modRate = float(m.rate);
                s.modBotplay = m.botplay;
                s.modPractice = m.practice;
                s.modMirror = m.mirror;
                s.modScrollType = m.scrollType;
                s.modScrollValue = float(m.scrollValue);
                a.saveSettings();
            }
            if (in.back() || in.pressed(KEY_TAB)) modsOpen_ = false;
            return;
        }
        if (n > 0) {
            if (in.up()) { sel = (sel + n - 1) % n; changed(); }
            if (in.down()) { sel = (sel + 1) % n; changed(); }
            const int nd = int(diffsOf(list_[size_t(sel)]).size());
            if (nd > 0) {
                if (in.left()) { a.freeplayDiff = (diffIndex() + nd - 1) % nd; sfxPlay(Sfx::Scroll); }
                if (in.right()) { a.freeplayDiff = (diffIndex() + 1) % nd; sfxPlay(Sfx::Scroll); }
            }
        }
        if (in.pressed(KEY_TAB)) { modsOpen_ = true; sfxPlay(Sfx::Scroll); }
        // Android: mod zips picked in the system file browser are copied to mods/.
        if (platform::kAndroid && in.pressed(KEY_I) && !importing_) {
            importing_ = platform::pickModZips();
            note(importing_ ? "Pick one or more mod .zip files" : "The file picker is not available");
        }
        bool rescan = in.pressed(KEY_R);
        if (importing_) {
            int count = 0;
            const platform::Import st = platform::importStatus(&count);
            if (st == platform::Import::Done || st == platform::Import::Nothing) {
                importing_ = false;
                platform::resetImport();
                if (st == platform::Import::Done) {
                    rescan = true;
                    note(TextFormat("Imported %d mod zip(s)", count));
                } else {
                    note("Nothing imported (mods must be .zip files)");
                }
            } else if (st == platform::Import::Copying) {
                note("Copying...");
            }
        }
        if (rescan) {
            const std::string id = n > 0 ? list_[size_t(sel)]->id : "";
            clearIconCache();
            a.library.scan(a.base, installProgress);
            rebuild();
            for (int i = 0; i < int(list_.size()); ++i)
                if (list_[size_t(i)]->id == id) sel = i;
            sfxPlay(Sfx::Confirm);
        }
        if (in.back()) { remember(); sfxPlay(Sfx::Cancel); a.change(makeMainMenu()); return; }
        if (in.pressed(KEY_M)) { remember(); sfxPlay(Sfx::Confirm); a.change(makeModPacks()); return; }
        if ((in.pressed(KEY_SEVEN) || in.pressed(KEY_KP_7)) && n > 0) {  // chart editor, as in Psych
            const SongEntry* e = list_[size_t(sel)];
            if (!e->diffs.empty()) {
                remember();
                sfxPlay(Sfx::Confirm);
                a.change(makeChartEditor(e->id, e->diffs[size_t(diffIndex())]));
                return;
            }
        }
        if (in.accept() && n > 0) {
            const SongEntry* e = list_[size_t(sel)];
            if (!diffsOf(e).empty()) {
                remember();
                sfxPlay(Sfx::Confirm);
                PlayRequest r;
                r.songId = e->id;
                r.diff = diffsOf(e)[size_t(diffIndex())];
                r.render = a.songListMode == App::SongListMode::Render;
                a.change(makePlay(r));
                return;
            }
        }
        // song preview
        if (n > 0) {
            const SongEntry* e = list_[size_t(sel)];
            if (!job_.running() && previewId_ != e->id && a.now - selChangedAt_ > 0.45) {
                pendingId_ = e->id;
                const SongEntry copy = *e;
                job_.start([copy] { return app().library.preview(copy); });
            }
            if (job_.ready()) {
                PcmPtr pcm = job_.take();
                if (pcm && pendingId_ == e->id) {
                    a.stopMenuMusic();
                    a.mixer.setTracks(pcm, nullptr, nullptr);
                    const double len = double(pcm->frames()) / Mixer::kRate;
                    const double start = e->builtin ? len * 0.38 : len * 0.3;
                    a.mixer.setLoop(true, start, len);
                    a.mixer.setRate(1.0);
                    a.mixer.play(start);
                    previewPlaying_ = true;
                }
                previewId_ = pendingId_;
            }
        }
    }

    void draw() override {
        App& a = app();
        const double now = a.now;
        gfx::beginCanvas();
        const unsigned hex = (unsigned(bg_.r) << 16) | (unsigned(bg_.g) << 8) | bg_.b;
        const float pulse = previewPlaying_ ? 0.0f : menuPulse();
        const int n = int(list_.size());
        if (a.psychMenusActive()) {
            const PsychUi& ui = psychUi();
            drawPsychMenuBackground(1, bg_, anim_ * 14.0f, true, now, hex, pulse);
            for (int i = std::max(0, int(anim_) - 5); i < std::min(n, int(anim_) + 6); ++i) {
                const float d = float(i) - anim_;
                const float y = 310.0f + d * 156.0f;
                const float x = 90.0f + d * 20.0f;
                if (y < -140.0f || y > gfx::H + 140.0f) continue;
                const bool on = i == a.freeplaySel;
                const float alpha = on ? 1.0f : 0.6f;
                const SongEntry* e = list_[size_t(i)];
                float textW = ui.alphabet(e->title, x, y, 64.0f, alpha);
                if (textW < 0) {
                    ui.uiText(e->title, x, y + 6.0f, 50.0f, fade(WHITE, alpha), -1, 2.5f);
                    textW = ui.textWidth(e->title, 50.0f);
                }
                const float iconX = std::min(x + textW + 74.0f, gfx::W - 110.0f);
                drawSongIcon(e, {iconX, y + 32.0f}, on ? 112.0f : 94.0f, now, alpha);
                if (on && !e->subtitle.empty())
                    ui.uiText(e->builtin ? e->subtitle : ("From: " + e->subtitle), x + 4.0f, y + 74.0f, 18.0f,
                              fade(WHITE, 0.85f), -1, 1.5f);
            }
            // Top-right Psych score + difficulty box.
            if (n > 0) {
                const SongEntry* e = list_[size_t(a.freeplaySel)];
                const bool hasDiffs = !diffsOf(e).empty();
                const std::string d = hasDiffs ? diffsOf(e)[size_t(diffIndex())] : std::string();
                const ScoreEntry* best = hasDiffs ? a.scores.get(Scores::key(scoreId(e), d)) : nullptr;
                const std::string scoreLine = best
                    ? ("PERSONAL BEST: " + fmtThousands(best->score) + TextFormat(" (%.0f%%)", best->acc * 100.0))
                    : std::string("PERSONAL BEST: 0 (0%)");
                const Modifiers& m = a.mods;
                const bool modOn = m.rate != 1.0 || m.botplay || m.practice || m.mirror || m.scrollType != 0;
                const float boxW = std::max(430.0f, ui.textWidth(scoreLine, 26.0f) + 34.0f);
                const float boxH = modOn ? 118.0f : 92.0f;
                DrawRectangleRec({gfx::W - boxW, 0, boxW, boxH}, Color{0, 0, 0, 155});
                const float cx = gfx::W - boxW * 0.5f;
                ui.uiText(scoreLine, gfx::W - 16.0f, 10.0f, 26.0f, WHITE, 1, 2.0f);
                if (hasDiffs) {
                    const std::string diffText = "< " + d + " >";
                    ui.uiText(diffText, cx, 48.0f, 24.0f, diffColor(d), 0, 2.0f);
                }
                if (modOn) {
                    std::string ms = TextFormat("Rate %.2fx", m.rate);
                    if (m.botplay) ms += "  Botplay";
                    if (m.practice) ms += "  Practice";
                    if (m.mirror) ms += "  Opponent";
                    if (m.scrollType == 1) ms += TextFormat("  Scroll x%.2f", m.scrollValue);
                    if (m.scrollType == 2) ms += TextFormat("  Scroll %.1f", m.scrollValue);
                    ui.uiText(ms, cx, 84.0f, 17.0f, kGold, 0, 1.5f);
                }
            }
            if (a.library.customCount() == 0) {
                const std::string tip = a.touchMode
                    ? "Tip: get mods in MOD PACKS, or copy them to " + platform::displayPath(a.base + "mods") + " and tap RESCAN"
                    : std::string("Tip: drop FNF / Psych Engine songs into the 'songs' or 'mods' folder, then press R");
                ui.uiText(tip, 24.0f, gfx::H - 72.0f, 18.0f, fade(WHITE, 0.8f), -1, 1.25f);
            }
            if (!note_.empty() && a.now - noteAt_ < 4.0)
                ui.uiText(note_, 24.0f, gfx::H - 102.0f, 22.0f, fade(kGold, float(std::min(1.0, 4.0 - (a.now - noteAt_)))), -1, 1.5f);
            if (typing_ || askingText_ || !query_.empty()) {
                const std::string shown = (query_.empty() && !typing_ ? std::string("...") : query_) +
                                          (typing_ && std::fmod(now, 1.0) < 0.5 ? "_" : "");
                DrawRectangleRec({16, 14, 720, 42}, Color{0, 0, 0, 190});
                ui.uiText("SEARCH", 30.0f, 23.0f, 20.0f, kGold, -1, 1.5f);
                ui.uiText(shown, 134.0f, 21.0f, 22.0f, WHITE, -1, 1.5f);
                const std::string count = n == 0 ? std::string("no songs") : TextFormat("%d song%s", n, n == 1 ? "" : "s");
                ui.uiText(count, 722.0f, 24.0f, 18.0f, fade(WHITE, 0.75f), 1, 1.25f);
            }
            if (a.songListMode == App::SongListMode::Render) {
                DrawRectangleRec({gfx::W - 430.0f, 124.0f, 430.0f, 40.0f}, Color{140, 24, 34, 215});
                ui.uiText("RENDER MODE", gfx::W - 215.0f, 131.0f, 24.0f, WHITE, 0, 2.0f);
            }
            if (modsOpen_) drawMods();
            const char* go = a.songListMode == App::SongListMode::Render ? "ENTER record" : "ENTER play";
            if (typing_) hints("Type the song's name   ENTER keep   BACKSPACE erase   ESC clear");
            else hints((query_.empty() ? std::string(go) + "   LEFT/RIGHT difficulty   F search   TAB modifiers   7 chart editor   M mod packs   R rescan   ESC back"
                                       : std::string(go) + "   LEFT/RIGHT difficulty   F search   ESC clear the search   TAB modifiers   7 chart editor")
                           .c_str());
            gfx::endCanvas();
            return;
        }
        drawMenuBackground(now, hex, pulse, true);
        for (int i = std::max(0, int(anim_) - 7); i < std::min(n, int(anim_) + 8); ++i) {
            const float d = float(i) - anim_;
            const float y = 330 + d * 76;
            const float x = 150 + d * 22;
            const bool on = i == a.freeplaySel;
            const float alpha = std::max(0.0f, 1.0f - std::fabs(d) * 0.2f);
            const SongEntry* e = list_[size_t(i)];
            drawSongIcon(e, {x - 50, y + 26}, on ? 70.0f : 56.0f, now, alpha);
            gfx::text(FontId::Ui, e->title, x, y, on ? 52.0f : 42.0f, fade(on ? WHITE : kIvory, alpha), Align::Left, 2.0f);
            if (on && !e->builtin)
                gfx::text(FontId::Ui, e->subtitle, x + 4, y + 50, 20, fade(kGold, 0.8f), Align::Left, 1.0f);
        }
        // info panel
        DrawRectangleRounded({830, 70, 410, 560}, 0.06f, 12, Color{8, 6, 16, 190});
        if (n > 0) {
            const SongEntry* e = list_[size_t(a.freeplaySel)];
            drawSongIcon(e, {1035, 175}, 150, now, 1.0f);
            gfx::text(FontId::Title, e->title, 1035, 262, e->title.size() > 16 ? 26.0f : 34.0f, kGold, Align::Center, 2.0f);
            float y = 312;
            for (const std::string& l : wrap(e->builtin ? e->subtitle : ("From: " + e->subtitle), 36)) {
                gfx::text(FontId::Ui, l, 1035, y, 22, kIvory, Align::Center);
                y += 24;
            }
            if (!diffsOf(e).empty()) {
                const std::string d = diffsOf(e)[size_t(diffIndex())];
                const Color dc = diffColor(d);
                const float bob = std::sin(float(now) * 4.0f) * 3.0f;
                gfx::drawNote(gfx::kLeft, {880 - bob, 430}, 40);
                gfx::drawNote(gfx::kRight, {1190 + bob, 430}, 40);
                gfx::text(FontId::Title, toLower(d) == "infernal" ? "INFERNAL" : d, 1035, 410, 34, dc, Align::Center, 2.0f);
                const ScoreEntry* best = a.scores.get(Scores::key(scoreId(e), d));
                const std::string bs = best ? fmtThousands(best->score) + "  -  " + TextFormat("%.2f%%", best->acc * 100.0) +
                                                  "  -  " + best->rank
                                            : "No record yet";
                gfx::text(FontId::Ui, "BEST", 1035, 466, 20, fade(kGold, 0.8f), Align::Center);
                gfx::text(FontId::Ui, bs, 1035, 488, 26, WHITE, Align::Center, 1.5f);
                gfx::text(FontId::Ui, TextFormat("Difficulty %d / %d", diffIndex() + 1, int(diffsOf(e).size())), 1035, 524, 20,
                          fade(kIvory, 0.6f), Align::Center);
            }
            const Modifiers& m = a.mods;
            std::string ms = TextFormat("Rate %.2fx", m.rate);
            if (m.botplay) ms += "  Botplay";
            if (m.practice) ms += "  Practice";
            if (m.mirror) ms += "  Opponent";
            if (m.scrollType == 1) ms += TextFormat("  Scroll x%.2f", m.scrollValue);
            if (m.scrollType == 2) ms += TextFormat("  Scroll %.1f", m.scrollValue);
            gfx::text(FontId::Ui, ms, 1035, 586, 22,
                      (m.rate != 1.0 || m.botplay || m.practice || m.mirror || m.scrollType != 0) ? kGold : fade(kIvory, 0.5f), Align::Center);
        }
        if (a.library.customCount() == 0) {
            const std::string tip = a.touchMode
                ? "Tip: get mods in MOD PACKS, or copy them to " + platform::displayPath(a.base + "mods") + " and tap RESCAN"
                : std::string("Tip: drop FNF / Psych Engine songs into the 'songs' or 'mods' folder, then press R");
            gfx::text(FontId::Ui, tip, 40, 640, 20, fade(kIvory, 0.65f));
        }
        if (!note_.empty() && a.now - noteAt_ < 4.0)
            gfx::text(FontId::Ui, note_, 40, 600, 24, fade(kGold, float(std::min(1.0, 4.0 - (a.now - noteAt_)))), Align::Left, 1.5f);
        if (typing_ || askingText_ || !query_.empty()) {
            const std::string shown = (query_.empty() && !typing_ ? std::string("...") : query_) +
                                      (typing_ && std::fmod(now, 1.0) < 0.5 ? "_" : "");
            DrawRectangleRounded({40, 22, 740, 42}, 0.4f, 8, Color{8, 6, 16, 210});
            gfx::text(FontId::Ui, "SEARCH", 58, 30, 22, kGold, Align::Left, 1.5f);
            gfx::text(FontId::Ui, shown, 160, 28, 26, WHITE, Align::Left, 1.0f);
            const std::string count = n == 0 ? std::string("no songs") : TextFormat("%d song%s", n, n == 1 ? "" : "s");
            gfx::text(FontId::Ui, count, 766, 32, 20, fade(kIvory, 0.7f), Align::Right, 1.0f);
        }
        if (a.songListMode == App::SongListMode::Render) {
            DrawRectangleRounded({830, 18, 410, 46}, 0.4f, 8, Color{120, 20, 30, 230});
            gfx::text(FontId::Title, "RENDER MODE", 1035, 20, 26, WHITE, Align::Center, 2.0f);
            gfx::text(FontId::Ui, "The song plays by itself and is saved as a video",
                      1035, 640, 18, fade(kIvory, 0.75f), Align::Center);
        }
        if (modsOpen_) drawMods();
        const char* go = a.songListMode == App::SongListMode::Render ? "ENTER record" : "ENTER play";
        if (typing_) hints("Type the song's name   ENTER keep   BACKSPACE erase   ESC clear");
        else hints((query_.empty() ? std::string(go) + "   LEFT/RIGHT difficulty   F search   TAB modifiers   7 chart editor   M mod packs   R rescan   ESC back"
                                   : std::string(go) + "   LEFT/RIGHT difficulty   F search   ESC clear the search   TAB modifiers   7 chart editor")
                       .c_str());
        gfx::endCanvas();
    }

private:
    void rebuild() {
        App& a = app();
        const std::string keep = !list_.empty() ? list_[size_t(clampv(a.freeplaySel, 0, int(list_.size()) - 1))]->id : std::string();
        list_.clear();
        const std::string q = toLower(query_);
        for (const SongEntry& e : a.library.songs())
            if (q.empty() || toLower(e.title).find(q) != std::string::npos || toLower(e.subtitle).find(q) != std::string::npos ||
                toLower(e.id).find(q) != std::string::npos)
                list_.push_back(&e);
        if (!keep.empty())  // the same song stays picked when it is still listed
            for (int i = 0; i < int(list_.size()); ++i)
                if (list_[size_t(i)]->id == keep) a.freeplaySel = i;
        // The first time after the game starts: the song picked last time.
        static bool restored = false;
        if (!restored && !a.settings.freeplaySong.empty()) {
            for (int i = 0; i < int(list_.size()); ++i)
                if (list_[size_t(i)]->id == a.settings.freeplaySong) a.freeplaySel = i;
        }
        restored = true;
        a.freeplaySel = clampv(a.freeplaySel, 0, std::max(0, int(list_.size()) - 1));
    }
    void setQuery(const std::string& q) {
        if (q == query_) return;
        query_ = q;
        rebuild();
        anim_ = float(app().freeplaySel);
        selChangedAt_ = app().now;
    }
    // The song and difficulty picked, kept for the next time the game starts.
    void remember() {
        App& a = app();
        if (list_.empty()) return;
        a.settings.freeplaySong = list_[size_t(clampv(a.freeplaySel, 0, int(list_.size()) - 1))]->id;
        a.settings.freeplayDiff = a.freeplayDiff;
        a.saveSettings();
    }
    void changed() {
        sfxPlay(Sfx::Scroll);
        selChangedAt_ = app().now;
    }
    void note(const std::string& s) {
        note_ = s;
        noteAt_ = app().now;
    }
    // Unpacking big zips takes a while: keep the screen alive.
    static void installProgress(const std::string& zip, double f) {
        BeginDrawing();
        ClearBackground(Color{12, 8, 20, 255});
        const char* t1 = TextFormat("Installing %s", zip.c_str());
        const int w = GetScreenWidth(), h = GetScreenHeight();
        DrawText(t1, (w - MeasureText(t1, 24)) / 2, h / 2 - 30, 24, RAYWHITE);
        DrawRectangle(w / 4, h / 2 + 10, w / 2, 10, Color{60, 50, 80, 255});
        DrawRectangle(w / 4, h / 2 + 10, int(w / 2 * f), 10, Color{200, 160, 255, 255});
        EndDrawing();
        PollInputEvents();
        SwapScreenBuffer();
    }
    static const std::vector<std::string>& diffsOf(const SongEntry* e) { return e->diffs; }
    static std::string scoreId(const SongEntry* e) { return e->id; }
    int diffIndex() const {
        if (list_.empty()) return 0;
        const int nd = int(diffsOf(list_[size_t(app().freeplaySel)]).size());
        return nd > 0 ? clampv(app().freeplayDiff, 0, nd - 1) : 0;
    }
    Color currentColor() const {
        if (list_.empty()) return kGold;
        return gfx::rgb(list_[size_t(app().freeplaySel)]->color);
    }
    void drawMods() {
        const App& a = app();
        const Modifiers& m = a.mods;
        const bool psych = a.psychMenusActive();
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 175});
        static const char* scrollTypes[3] = {"Chart", "Multiplicative", "Constant"};
        if (psych) {
            const PsychUi& ui = psychUi();
            // Psych Engine GameplayChangersSubstate: Alphabet list + animated CheckboxThingie.
            if (ui.alphabetCentered("GAMEPLAY CHANGERS", gfx::W * 0.5f, 36.0f, 52.0f, 1.0f) < 0)
                ui.uiText("GAMEPLAY CHANGERS", gfx::W * 0.5f, 40.0f, 42.0f, WHITE, 0, 2.0f);
            struct ModRow {
                const char* label;
                bool isBool;
                bool bval;
                std::string sval;
                bool dimmed;
            };
            const ModRow items[kModRows] = {
                {"Playback Rate", false, false, TextFormat("%.2fx", m.rate), false},
                {"Botplay", true, m.botplay, {}, false},
                {"Practice Mode", true, m.practice, {}, false},
                {"Opponent Play", true, m.mirror, {}, false},
                {"Scroll Type", false, false, scrollTypes[m.scrollType], false},
                {"Scroll Speed", false, false,
                 m.scrollType == 0 ? std::string("-")
                                   : std::string(TextFormat(m.scrollType == 1 ? "x%.2f" : "%.1f", m.scrollValue)),
                 m.scrollType == 0},
            };
            for (int i = 0; i < kModRows; ++i) {
                const bool on = i == modSel_;
                const float alpha = items[i].dimmed ? 0.35f : (on ? 1.0f : 0.6f);
                const float y = 136.0f + float(i) * 80.0f;
                const float x = items[i].isBool ? 290.0f : 210.0f;
                if (items[i].isBool) {
                    const double el = (lastModToggleRow_ == i) ? (a.now - lastModToggleAt_) : -1.0;
                    if (!ui.drawCheckbox({x - 110.0f, y - 10.0f}, items[i].bval, el, 0.68f, alpha))
                        ui.uiText(items[i].bval ? "[X]" : "[ ]", x - 90.0f, y + 6.0f, 32.0f, fade(WHITE, alpha), 0, 2.0f);
                }
                float lw = ui.alphabet(items[i].label, x, y, 52.0f, alpha, false);
                if (lw < 0) {
                    ui.uiText(items[i].label, x, y + 10.0f, 32.0f, fade(WHITE, alpha), -1, 2.0f);
                    lw = ui.textWidth(items[i].label, 32.0f);
                }
                if (!items[i].isBool) {
                    const float vx = x + lw + 48.0f;
                    if (ui.alphabet(items[i].sval, vx, y, 50.0f, alpha, true) < 0)
                        ui.uiText(items[i].sval, vx, y + 10.0f, 32.0f, fade(on ? kGold : WHITE, alpha), -1, 2.0f);
                }
            }
            DrawRectangleRec({110, gfx::H - 96, gfx::W - 220, 48}, Color{0, 0, 0, 175});
            ui.uiText("Scores are only saved at 1.00x or faster, without botplay or practice", gfx::W * 0.5f, gfx::H - 82.0f, 20.0f,
                      fade(WHITE, 0.9f), 0, 1.5f);
            return;
        }
        DrawRectangleRounded({370, 140, 540, 430}, 0.08f, 12, Color{16, 10, 30, 245});
        const std::string rows[kModRows] = {
            TextFormat("Playback rate   %.2fx", m.rate), std::string("Botplay   ") + (m.botplay ? "ON" : "OFF"),
            std::string("Practice (no death)   ") + (m.practice ? "ON" : "OFF"),
            std::string("Play as the opponent   ") + (m.mirror ? "ON" : "OFF"), std::string("Scroll speed   ") + scrollTypes[m.scrollType],
            m.scrollType == 0 ? std::string("Scroll value   -")
                              : std::string(TextFormat(m.scrollType == 1 ? "Scroll value   x%.2f" : "Scroll value   %.1f", m.scrollValue))};
        gfx::text(FontId::Title, "MODIFIERS", 640, 158, 38, kGold, Align::Center, 2.0f);
        for (int i = 0; i < kModRows; ++i)
            gfx::text(FontId::Ui, rows[i], 640, 222.0f + i * 50, 30, i == modSel_ ? kGold : (i == 5 && m.scrollType == 0 ? fade(kIvory, 0.4f) : kIvory),
                      Align::Center, 1.5f);
        gfx::text(FontId::Ui, "Scores are only saved at 1.00x or faster, without botplay or practice", 640, 532, 18,
                  fade(kIvory, 0.6f), Align::Center);
    }

    struct IconEntry {
        Texture2D tex{};
        int frames = 1;
        bool hasIcon = false;
        uint64_t stamp = 0;
    };

    void clearIconCache() {
        for (auto& [k, e] : iconCache_)
            if (e.tex.id) UnloadTexture(e.tex);
        iconCache_.clear();
        iconLoadedCount_ = 0;
    }

    const IconEntry* iconForSong(const SongEntry* e) {
        if (!e || e->builtin || e->chartOpponent.empty()) return nullptr;
        const std::string key = e->pack + "|" + e->modRoot + "|" + toLower(e->chartOpponent);
        auto it = iconCache_.find(key);
        if (it != iconCache_.end()) {
            it->second.stamp = ++iconTick_;
            return it->second.hasIcon ? &it->second : nullptr;
        }
        IconEntry entry;
        entry.stamp = ++iconTick_;
        const App& a = app();
        scene::Files files;
        std::vector<std::string> roots = e->assetRoots;
        if (!e->modRoot.empty()) roots.push_back(e->modRoot);
        roots.push_back(a.base + "mods/");
        files.setRoots(std::move(roots));
        std::vector<std::string> shared = a.library.sharedAssetRootsFor(*e);
        if (!a.psychFolderInUse.empty()) {
            shared.push_back(a.psychFolderInUse);
            shared.push_back(a.psychFolderInUse + "assets/");
            shared.push_back(a.psychFolderInUse + "assets/shared/");
            shared.push_back(a.psychFolderInUse + "assets/preload/");
        }
        files.setSharedRoots(std::move(shared));

        std::string iconName = e->chartOpponent;
        std::string charJson;
        std::string charPath = files.find("characters/" + e->chartOpponent + ".json");
        if (charPath.empty()) charPath = files.find("data/characters/" + e->chartOpponent + ".json");
        if (!charPath.empty()) scene::readFile(charPath, charJson);
        if (charJson.empty()) {
            if (const char* bg = basegame::characterJson(e->chartOpponent)) charJson = bg;
        }
        if (!charJson.empty()) {
            psych::CharacterDef def;
            std::string charErr;
            if (psych::parseCharacter(charJson, def, charErr) && !def.icon.empty()) iconName = def.icon;
        }

        std::string iconPath = files.find("images/icons/icon-" + iconName + ".png");
        if (iconPath.empty()) iconPath = files.find("images/icons/" + iconName + ".png");
        if (iconPath.empty() && toLower(iconName) != toLower(e->chartOpponent)) {
            iconPath = files.find("images/icons/icon-" + e->chartOpponent + ".png");
            if (iconPath.empty()) iconPath = files.find("images/icons/" + e->chartOpponent + ".png");
        }
        if (!iconPath.empty()) {
            std::string bytes;
            if (scene::readFile(iconPath, bytes) && !bytes.empty()) {
                Image im = LoadImageFromMemory(".png", reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()));
                if (im.data && im.width > 0 && im.height > 0) {
                    // Evict oldest loaded texture if we hit the cache cap.
                    if (iconLoadedCount_ >= 64) {
                        auto oldest = iconCache_.end();
                        for (auto jt = iconCache_.begin(); jt != iconCache_.end(); ++jt)
                            if (jt->second.tex.id && (oldest == iconCache_.end() || jt->second.stamp < oldest->second.stamp))
                                oldest = jt;
                        if (oldest != iconCache_.end()) {
                            UnloadTexture(oldest->second.tex);
                            iconCache_.erase(oldest);
                            --iconLoadedCount_;
                        }
                    }
                    entry.tex = LoadTextureFromImage(im);
                    if (entry.tex.id) {
                        SetTextureFilter(entry.tex, a.settings.antialiasing ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
                        entry.frames = std::max(1, int(std::lround(double(im.width) / std::max(1, im.height))));
                        entry.hasIcon = true;
                        ++iconLoadedCount_;
                    }
                }
                if (im.data) UnloadImage(im);
            }
        }
        auto [ins, _] = iconCache_.emplace(key, entry);
        return ins->second.hasIcon ? &ins->second : nullptr;
    }

    void drawSongIcon(const SongEntry* e, Vector2 pos, float size, double now, float alpha) {
        if (const IconEntry* ic = iconForSong(e)) {
            const float fw = float(ic->tex.width) / float(ic->frames);
            const float fh = float(ic->tex.height);
            const float sc = size / std::max(1.0f, std::max(fw, fh));
            const float w = fw * sc, h = fh * sc;
            DrawTexturePro(ic->tex, {0, 0, fw, fh}, {pos.x - w * 0.5f, pos.y - h * 0.5f, w, h}, {0, 0}, 0, fade(WHITE, alpha));
            return;
        }
        const CharId cid = e->builtin ? charFromName(e->opponent) : CharId::Shade;
        drawIcon(cid, gfx::rgb(e->color), pos, size, false, now);
    }

    std::vector<const SongEntry*> list_;
    std::map<std::string, IconEntry> iconCache_;
    uint64_t iconTick_ = 0;
    int iconLoadedCount_ = 0;
    float anim_ = 0;
    Color bg_ = kGold;
    double selChangedAt_ = 0;
    PcmJob job_;
    std::string pendingId_, previewId_;
    bool previewPlaying_ = false;
    bool modsOpen_ = false;
    static constexpr int kModRows = 6;
    int modSel_ = 0;
    int lastModToggleRow_ = -1;
    double lastModToggleAt_ = -10.0;
    bool importing_ = false;
    std::string query_;        // the search (empty: every song)
    bool typing_ = false;      // typing it with the keyboard
    bool askingText_ = false;  // Android: the system's text box is open
    std::string note_;
    double noteAt_ = 0;
};

// ---------------------------------------------------------------- Credits --

class CreditsState : public State {
public:
    void enter() override { t0_ = app().now; }
    void update(double) override {
        if (app().input.back() || app().input.accept()) {
            sfxPlay(Sfx::Cancel);
            app().change(makeMainMenu());
        }
    }
    void draw() override {
        static const char* lines[] = {
            "#ASTRALIS",
            "Astralis Engine v" GH_VERSION,
            "",
            "@ASTRALIS ENGINE",
            "A native C++20 rhythm game engine by deatuamae67-crypto",
            "High-performance multi-key, Psych Lua & spam chart engine",
            "",
            "@ASSETS & DEFAULT SONGS",
            "Psych Engine 0.7.3 UI, Tutorial & Week 1 assets",
            "ShadowMario & Psych Engine contributors (Apache 2.0)",
            "Friday Night Funkin' original cast & music by The Funkin' Crew",
            "",
            "@LIBRARIES",
            "raylib 5.5 by Ramon Santamaria (zlib license)",
            "Lua 5.4 by PUC-Rio (MIT license)",
            "miniz by Rich Geldreich (MIT license)",
            "",
            "@FONTS",
            "VCR OSD Mono by Riciery Leal",
            "",
            "@LICENSE",
            "See LICENSE and licenses/PSYCH_ENGINE_LICENSE.txt",
            "Play it, chart for it, mod it and build with it",
            "",
            "Thank you for playing Astralis.",
        };
        const App& a = app();
        const double now = a.now;
        gfx::beginCanvas();
        const bool psych = a.psychMenusActive();
        if (psych) {
            drawPsychMenuBackground(1, Color{108, 140, 255, 255}, 0, false, now, 0x6C8CFF, menuPulse());
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 110});
        } else {
            drawMenuBackground(now, 0x6C8CFF, menuPulse());
        }
        float y = 640 - float(now - t0_) * 42.0f;
        const float total = float(sizeof(lines) / sizeof(lines[0])) * 46 + 700;
        while (y < -total + 640) y += total;
        const PsychUi& ui = psychUi();
        for (const char* l : lines) {
            if (psych) {
                if (l[0] == '#') {
                    if (ui.alphabetCentered(l + 1, gfx::W * 0.5f, y, 58.0f, 1.0f) < 0)
                        ui.uiText(l + 1, gfx::W * 0.5f, y, 56.0f, kGold, 0, 2.5f);
                } else if (l[0] == '@') {
                    if (ui.alphabetCentered(l + 1, gfx::W * 0.5f, y + 6.0f, 34.0f, 1.0f) < 0)
                        ui.uiText(l + 1, gfx::W * 0.5f, y + 10.0f, 30.0f, kGold, 0, 2.0f);
                } else if (l[0] != '\0') {
                    ui.uiText(l, gfx::W * 0.5f, y + 10.0f, 24.0f, WHITE, 0, 1.75f);
                }
            } else {
                if (l[0] == '#') gfx::glowText(FontId::Title, l + 1, gfx::W / 2, y, 64, kGold, Color{255, 180, 80, 255});
                else if (l[0] == '@') gfx::text(FontId::Title, l + 1, gfx::W / 2, y + 10, 30, kGold, Align::Center, 2.0f);
                else gfx::text(FontId::Ui, l, gfx::W / 2, y + 10, 28, kIvory, Align::Center, 1.5f);
            }
            y += l[0] == '#' ? 80 : 46;
        }
        hints("ESC back");
        gfx::endCanvas();
    }

private:
    double t0_ = 0;
};

std::unique_ptr<State> makeTitle() { return std::make_unique<TitleState>(); }
std::unique_ptr<State> makeMainMenu() { return std::make_unique<MainMenuState>(); }
std::unique_ptr<State> makeStory() { return std::make_unique<StoryState>(); }
std::unique_ptr<State> makeFreeplay() { return std::make_unique<FreeplayState>(); }
std::unique_ptr<State> makeCredits() { return std::make_unique<CreditsState>(); }

}  // namespace gh
