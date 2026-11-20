// God's Hand of Principality and Demise. Copyright (c) 2026 deatuamae67-crypto.
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
#include "video.hpp"

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
    gfx::text(FontId::Ui, s, gfx::W / 2, gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
}

double lastBeatTime() {
    const double b = app().menuBeat();
    const double frac = b - std::floor(b);
    return app().now - frac * 60.0 / app().menuBpm();
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

// ------------------------------------------------------------------ Intro --

// The engine's intro (issue #31): assets/videos/intro.mpg before the title,
// full screen with its sound (MPEG-1, which every platform plays). ENTER skips
// it on a computer (Space, ESC and the gamepad's buttons too); on a
// touchscreen the SKIP button does, and so does the back button.
std::string introPath() { return app().base + "assets/videos/intro.mpg"; }

class IntroState : public State {
public:
    bool wantsMenuMusic() const override { return false; }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        t.keys = {{"SKIP", KEY_ENTER}};
        return t;
    }
    void enter() override {
        App& a = app();
        shownAt_ = a.now;
        if (video_.open(introPath(), false, true)) {
            video_.setVolume(a.settings.musicVol);
            platform::log("intro " + std::to_string(video_.width()) + "x" + std::to_string(video_.height()) + ", " +
                          std::to_string(int(video_.duration() * 1000)) + " ms");
        } else {
            leave();
        }
    }
    void update(double dt) override {
        App& a = app();
        if (leaving_) return;
        video_.update(std::min(dt, 0.1));  // a hitch does not jump ahead
        if (a.input.accept() || a.input.back()) {
            platform::log("intro skipped at " + std::to_string(int((a.now - shownAt_) * 1000)) + " ms");
            leave();
        } else if (video_.ended()) {
            leave();
        }
    }
    void draw() override {
        App& a = app();
        gfx::beginCanvas();
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, BLACK);
        if (video_.width() > 0 && video_.height() > 0) {  // the whole picture, as large as it fits
            const float vw = float(video_.width()), vh = float(video_.height());
            const float s = std::min(gfx::W / vw, gfx::H / vh);
            DrawTexturePro(video_.texture(), {0, 0, vw, vh}, {(gfx::W - vw * s) * 0.5f, (gfx::H - vh * s) * 0.5f, vw * s, vh * s}, {0, 0},
                           0, WHITE);
        }
        if (!a.touchMode) {  // the way out, after a moment
            const float al = saturate((float(a.now - shownAt_) - 0.6f) / 0.4f) * 0.8f;
            if (al > 0.01f) gfx::text(FontId::Ui, "ENTER  skip", gfx::W - 28, gfx::H - 48, 26, fade(WHITE, al), Align::Right, 2.0f);
        }
        gfx::endCanvas();
    }

private:
    void leave() {
        leaving_ = true;
        video_.setVolume(0);  // the picture stays while the screen fades
        app().change(makeTitle());
    }
    VideoPlayer video_;
    bool leaving_ = false;
    double shownAt_ = 0;
};

// ------------------------------------------------------------------ Title --

class TitleState : public State {
public:
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        t.tapCode = KEY_ENTER;
        return t;
    }
    void update(double) override {
        App& a = app();
        flash_ = std::max(0.0f, flash_ - float(a.dt) * 1.8f);
        if (!leaving_ && (a.input.accept() || a.input.firstPressed() != 0) && !a.input.pressed(KEY_F11)) {
            leaving_ = true;
            leaveAt_ = a.now;
            flash_ = 1.0f;
            sfxPlay(Sfx::Confirm);
        }
        if (leaving_ && a.now - leaveAt_ > 0.75) a.change(makeMainMenu());
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        const float pulse = menuPulse();
        gfx::beginCanvas();
        if (a.psychMenusActive()) {
            const PsychUi& ui = psychUi();
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, BLACK);
            gfx::glow({gfx::W * 0.5f, gfx::H * 0.44f}, 320.0f + 50.0f * pulse, fade(Color{110, 90, 210, 255}, 0.22f + 0.12f * pulse));
            const float s = 1.0f + 0.045f * pulse;
            if (ui.alphabetCentered("GOD'S HAND", gfx::W * 0.5f, 165.0f - 10.0f * pulse, 84.0f * s, 1.0f) < 0)
                ui.uiText("GOD'S HAND", gfx::W * 0.5f, 155.0f - 8.0f * pulse, 86.0f * s, WHITE, 0, 3.0f);
            if (ui.alphabetCentered("OF PRINCIPALITY AND DEMISE", gfx::W * 0.5f, 286.0f, 34.0f, 0.9f) < 0)
                ui.uiText("OF PRINCIPALITY AND DEMISE", gfx::W * 0.5f, 288.0f, 28.0f, kIvory, 0, 2.0f);
            const float blink = leaving_ ? (std::fmod(float(now) * 14.0f, 2.0f) < 1.0f ? 1.0f : 0.15f)
                                         : 0.45f + 0.55f * (0.5f + 0.5f * std::sin(float(now) * 3.5f));
            const char* prompt = a.touchMode ? "TAP TO BEGIN" : "PRESS ENTER TO BEGIN";
            const Color pc = leaving_ ? Color{51, 255, 255, 255} : Color{51, 51, 204, 255};
            ui.uiText(prompt, gfx::W * 0.5f, 520.0f, 38.0f, fade(leaving_ ? pc : gfx::mix(pc, WHITE, blink), blink), 0, 2.5f);
            ui.uiText("God's Hand Engine v" GH_VERSION, 14.0f, gfx::H - 34.0f, 18.0f, fade(WHITE, 0.75f), -1, 1.5f);
            if (!a.mixer.audioOk())
                ui.uiText("No audio device found - the game will run silently", gfx::W * 0.5f, 580.0f, 20.0f,
                          Color{255, 160, 140, 255}, 0, 1.5f);
            if (flash_ > 0 && a.settings.flashing) DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(WHITE, flash_ * 0.85f));
            gfx::endCanvas();
            return;
        }
        drawMenuBackground(now, 0xF4C76B, pulse);
        CharacterState p;
        p.id = CharId::Principality;
        p.beatAt = lastBeatTime();
        drawCharacter(p, {230, 690}, 0.9f, now, false);
        CharacterState h;
        h.id = CharId::Herald;
        h.beatAt = p.beatAt;
        drawCharacter(h, {1060, 690}, 0.95f, now, true);
        const float s = 1.0f + 0.025f * pulse;
        gfx::glowText(FontId::Title, "GOD'S HAND", gfx::W / 2, 150 - 6 * s, 104 * s, kGold, Color{255, 190, 80, 255});
        gfx::text(FontId::Title, "of Principality and Demise", gfx::W / 2, 286, 38, kIvory, Align::Center, 2.0f);
        const float blink = leaving_ ? (std::fmod(float(now) * 12.0f, 2.0f) < 1.0f ? 1.0f : 0.2f)
                                     : 0.55f + 0.45f * std::sin(float(now) * 3.5f);
        gfx::text(FontId::Ui, a.touchMode ? "TAP TO BEGIN" : "PRESS ENTER TO BEGIN", gfx::W / 2, 470, 40, fade(WHITE, blink),
                  Align::Center, 2.0f);
        gfx::text(FontId::Ui, "A rhythm duel  -  v" GH_VERSION, gfx::W / 2, 680, 22, fade(kIvory, 0.7f), Align::Center, 1.0f);
        if (!a.mixer.audioOk())
            gfx::text(FontId::Ui, "No audio device found - the game will run silently", gfx::W / 2, 520, 22,
                      Color{255, 160, 140, 255}, Align::Center, 1.5f);
        if (flash_ > 0) DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(WHITE, flash_ * 0.8f));
        gfx::endCanvas();
    }

private:
    bool leaving_ = false;
    double leaveAt_ = 0;
    float flash_ = 0;
};

// -------------------------------------------------------------- Main menu --

// RENDER MODE and AUTOCHART open the song list for recording a video or
// for playing a chart made from the song's audio.
constexpr const char* kMainItems[] = {"STORY MODE", "FREEPLAY", "RENDER MODE", "AUTOCHART", "OPTIONS", "CREDITS", "QUIT"};
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
                    case 2:
                    case 3:
                        a.songListMode = chosen_ == 2   ? App::SongListMode::Render
                                         : chosen_ == 3 ? App::SongListMode::AutoChart
                                                        : App::SongListMode::Play;
                        a.change(makeFreeplay());
                        break;
                    case 4: a.change(makeOptions()); break;
                    case 5: a.change(makeCredits()); break;
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
        // 7: the editors (chart, character, stage, events), as Psych's Master Editor Menu.
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
            drawPsychMenuBackground(magenta ? 3 : 0, WHITE, anim_ * 38.0f, true, now, 0xFDE871, pulse);
            for (int i = 0; i < kMainCount; ++i) {
                const bool on = i == a.mainSel;
                const float d = float(i) - anim_;
                const float y = 330.0f + d * 116.0f;
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
                else if (i == 4) spriteIdx = 2;  // options
                else if (i == 5) spriteIdx = 3;  // credits
                bool drawn = false;
                if (spriteIdx >= 0) drawn = ui.drawMainMenuItem(spriteIdx, {gfx::W * 0.5f, y}, on, now, itemAlpha);
                if (!drawn) {
                    const float h = on ? 72.0f : 56.0f;
                    if (ui.alphabetCentered(items[i], gfx::W * 0.5f, y - h * 0.5f, h, itemAlpha) < 0)
                        ui.uiText(items[i], gfx::W * 0.5f, y - h * 0.5f, on ? 56.0f : 44.0f,
                                  fade(on ? kGold : WHITE, itemAlpha), 0, 2.5f);
                }
            }
            ui.uiText("God's Hand Engine v" GH_VERSION, 12.0f, gfx::H - 88.0f, 18.0f, WHITE, -1, 1.5f);
            ui.uiText(a.library.report(), 12.0f, gfx::H - 66.0f, 16.0f, fade(WHITE, 0.85f), -1, 1.25f);
            hints("UP/DOWN select   ENTER confirm   7 editors   ESC back   F11 fullscreen");
            gfx::endCanvas();
            return;
        }
        drawMenuBackground(now, 0x8C6CFF, pulse, true);
        CharacterState h;
        h.id = CharId::Herald;
        h.beatAt = lastBeatTime();
        gfx::glow({1080, 500}, 230 + 20 * pulse, fade(kGold, 0.3f));
        drawCharacter(h, {1080, 640}, 0.85f, now, true);
        gfx::text(FontId::Title, "GOD'S HAND", 60, 40, 44, kGold, Align::Left, 2.0f);
        for (int i = 0; i < kMainCount; ++i) {
            const bool on = i == a.mainSel;
            const float y = 128.0f + i * 70.0f - (on ? 6.0f : 0.0f);
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
    void update(double) override {
        App& a = app();
        static const char* diffs[3] = {"Easy", "Normal", "Hard"};
        int& d = a.storyDiff;
        d = clampv(d, 0, 2);
        if (a.input.left()) { d = (d + 2) % 3; sfxPlay(Sfx::Scroll); }
        if (a.input.right()) { d = (d + 1) % 3; sfxPlay(Sfx::Scroll); }
        if (a.input.back()) { sfxPlay(Sfx::Cancel); a.change(makeMainMenu()); }
        if (a.input.accept() && !go_) {
            go_ = true;
            sfxPlay(Sfx::Confirm);
            PlayRequest r;
            r.songId = "builtin:principality";
            r.diff = diffs[d];
            r.story = true;
            r.storyQueue = {"builtin:godshand", "builtin:demise"};
            a.change(makePlay(r));
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        static const char* diffs[3] = {"Easy", "Normal", "Hard"};
        const std::string d = diffs[clampv(a.storyDiff, 0, 2)];
        const ScoreEntry* best = a.scores.get(Scores::key("week:ascension", d));
        gfx::beginCanvas();
        if (a.psychMenusActive()) {
            const PsychUi& ui = psychUi();
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, BLACK);
            ui.uiText(best ? ("WEEK SCORE: " + fmtThousands(best->score)) : std::string("WEEK SCORE: 0"), 18.0f, 12.0f, 30.0f, WHITE, -1,
                      2.0f);
            ui.uiText("THE BIG SHOW", gfx::W - 18.0f, 12.0f, 30.0f, fade(WHITE, 0.72f), 1, 2.0f);
            DrawRectangleRec({0, 56, gfx::W, 400}, Color{249, 207, 81, 255});
            const CharId ids[3] = {CharId::Principality, CharId::Hand, CharId::Demise};
            const double bt = lastBeatTime();
            for (int i = 0; i < 3; ++i) {
                CharacterState c;
                c.id = ids[i];
                c.beatAt = bt;
                const float x = 310.0f + i * 330.0f;
                drawCharacter(c, {x, 424}, i == 2 ? 0.68f : 0.74f, now, false);
                ui.uiText(charDisplayName(ids[i]), x, 424.0f, 20.0f, WHITE, 0, 2.0f);
            }
            const Color pink{229, 87, 119, 255};
            ui.uiText("TRACKS", 190.0f, 476.0f, 30.0f, pink, 0, 2.0f);
            const char* tracks[3] = {"Principality", "Godshand", "Demise"};
            for (int i = 0; i < 3; ++i) ui.uiText(tracks[i], 190.0f, 520.0f + i * 36.0f, 26.0f, pink, 0, 1.5f);
            if (ui.alphabetCentered("WEEK 1", 640.0f, 514.0f, 56.0f, 1.0f) < 0)
                ui.uiText("WEEK 1", 640.0f, 514.0f, 48.0f, WHITE, 0, 2.5f);
            const Color dc = diffColor(d);
            const float bob = std::sin(float(now) * 4.0f) * 4.0f;
            const std::string diffUpper = toLower(d) == "hard" ? "HARD" : (d == "Easy" ? "EASY" : "NORMAL");
            ui.uiText("<", 890.0f - bob, 518.0f, 44.0f, WHITE, 0, 2.0f);
            ui.uiText(diffUpper, 1040.0f, 518.0f, 44.0f, dc, 0, 2.5f);
            ui.uiText(">", 1190.0f + bob, 518.0f, 44.0f, WHITE, 0, 2.0f);
            hints("LEFT/RIGHT difficulty   ENTER start the week   ESC back");
            gfx::endCanvas();
            return;
        }
        drawMenuBackground(now, 0xFF8A5C, menuPulse());
        gfx::gradientRect({0, 70, gfx::W, 330}, Color{10, 6, 20, 220}, Color{40, 18, 40, 220});
        const CharId ids[3] = {CharId::Principality, CharId::Hand, CharId::Demise};
        const double bt = lastBeatTime();
        for (int i = 0; i < 3; ++i) {
            CharacterState c;
            c.id = ids[i];
            c.beatAt = bt;
            const float x = 330.0f + i * 310.0f;
            drawCharacter(c, {x, 370}, i == 2 ? 0.62f : 0.68f, now, false);
            gfx::text(FontId::Ui, charDisplayName(ids[i]), x, 372, 24, charColor(ids[i]), Align::Center, 1.5f);
        }
        gfx::text(FontId::Title, "WEEK 1", 60, 14, 40, kGold, Align::Left, 2.0f);
        gfx::text(FontId::Title, "THE BIG SHOW", gfx::W - 60, 14, 40, kIvory, Align::Right, 2.0f);
        gfx::text(FontId::Ui, "TRACKS", 120, 440, 28, kGold);
        const char* tracks[3] = {"Principality", "Godshand", "Demise"};
        for (int i = 0; i < 3; ++i) gfx::text(FontId::Ui, tracks[i], 120, 478.0f + i * 40, 34, kIvory, Align::Left, 1.5f);
        const Color dc = diffColor(d);
        const float bob = std::sin(float(now) * 4.0f) * 4.0f;
        gfx::drawNote(gfx::kLeft, {760 - bob, 500}, 54);
        gfx::drawNote(gfx::kRight, {1160 + bob, 500}, 54);
        gfx::glowText(FontId::Title, toLower(d) == "hard" ? "HARD" : (d == "Easy" ? "EASY" : "NORMAL"), 960, 470, 56, dc, dc);
        gfx::text(FontId::Ui, best ? ("WEEK SCORE: " + fmtThousands(best->score)) : std::string("WEEK SCORE: -"), 960, 560,
                  28, kIvory, Align::Center, 1.5f);
        hints("LEFT/RIGHT difficulty   ENTER start the week   ESC back");
        gfx::endCanvas();
    }

private:
    bool go_ = false;
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
                if (modSel_ == 1) m.botplay = !m.botplay;
                if (modSel_ == 2) m.practice = !m.practice;
                if (modSel_ == 3) m.mirror = !m.mirror;
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
                r.autoChart = autoMode();
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
            if (a.songListMode != App::SongListMode::Play) {
                const bool render = a.songListMode == App::SongListMode::Render;
                DrawRectangleRec({gfx::W - 430.0f, 124.0f, 430.0f, 40.0f}, render ? Color{140, 24, 34, 215} : Color{24, 76, 140, 215});
                ui.uiText(render ? "RENDER MODE" : "AUTOCHART", gfx::W - 215.0f, 131.0f, 24.0f, WHITE, 0, 2.0f);
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
        if (a.songListMode != App::SongListMode::Play) {
            const bool render = a.songListMode == App::SongListMode::Render;
            DrawRectangleRounded({830, 18, 410, 46}, 0.4f, 8, render ? Color{120, 20, 30, 230} : Color{20, 70, 120, 230});
            gfx::text(FontId::Title, render ? "RENDER MODE" : "AUTOCHART", 1035, 20, 26, WHITE, Align::Center, 2.0f);
            gfx::text(FontId::Ui,
                      render ? "The song plays by itself and is saved as a video"
                             : "A chart made from the song's own audio",
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
    // AUTOCHART: every song in the three difficulties AutoChart makes.
    static bool autoMode() { return app().songListMode == App::SongListMode::AutoChart; }
    static const std::vector<std::string>& diffsOf(const SongEntry* e) {
        static const std::vector<std::string> kAuto = {"Easy", "Normal", "Hard"};
        return autoMode() ? kAuto : e->diffs;
    }
    static std::string scoreId(const SongEntry* e) { return autoMode() ? e->id + "#autochart" : e->id; }
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
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{0, 0, 0, 165});
        if (psych) DrawRectangleRec({340, 130, 600, 450}, Color{0, 0, 0, 225});
        else DrawRectangleRounded({370, 140, 540, 430}, 0.08f, 12, Color{16, 10, 30, 245});
        static const char* scrollTypes[3] = {"the chart's", "multiplied", "constant"};
        const std::string rows[kModRows] = {
            TextFormat("Playback rate   %.2fx", m.rate), std::string("Botplay   ") + (m.botplay ? "ON" : "OFF"),
            std::string("Practice (no death)   ") + (m.practice ? "ON" : "OFF"),
            std::string("Play as the opponent   ") + (m.mirror ? "ON" : "OFF"), std::string("Scroll speed   ") + scrollTypes[m.scrollType],
            m.scrollType == 0 ? std::string("Scroll value   -")
                              : std::string(TextFormat(m.scrollType == 1 ? "Scroll value   x%.2f" : "Scroll value   %.1f", m.scrollValue))};
        if (psych) {
            const PsychUi& ui = psychUi();
            if (ui.alphabetCentered("GAMEPLAY CHANGERS", 640.0f, 146.0f, 38.0f, 1.0f) < 0)
                ui.uiText("GAMEPLAY CHANGERS", 640.0f, 150.0f, 34.0f, WHITE, 0, 2.0f);
            for (int i = 0; i < kModRows; ++i)
                ui.uiText(rows[i], 640.0f, 220.0f + i * 50.0f, 26.0f,
                          i == modSel_ ? kGold : (i == 5 && m.scrollType == 0 ? fade(WHITE, 0.4f) : WHITE), 0, 2.0f);
            ui.uiText("Scores are only saved at 1.00x or faster, without botplay or practice", 640.0f, 536.0f, 16.0f,
                      fade(WHITE, 0.7f), 0, 1.25f);
        } else {
            gfx::text(FontId::Title, "MODIFIERS", 640, 158, 38, kGold, Align::Center, 2.0f);
            for (int i = 0; i < kModRows; ++i)
                gfx::text(FontId::Ui, rows[i], 640, 222.0f + i * 50, 30, i == modSel_ ? kGold : (i == 5 && m.scrollType == 0 ? fade(kIvory, 0.4f) : kIvory),
                          Align::Center, 1.5f);
            gfx::text(FontId::Ui, "Scores are only saved at 1.00x or faster, without botplay or practice", 640, 532, 18,
                      fade(kIvory, 0.6f), Align::Center);
        }
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
            "#GOD'S HAND",
            "of Principality and Demise",
            "",
            "@A RHYTHM DUEL",
            "A project by deatuamae67-crypto",
            "",
            "@ENGINE, MUSIC, ART AND CHARTS",
            "Written from scratch in C++20",
            "Every song is synthesized and every character is drawn in code",
            "Built with Claude Code",
            "",
            "@LIBRARIES",
            "raylib 5.5 by Ramon Santamaria (zlib license)",
            "",
            "@FONTS",
            "Luckiest Guy by Astigmatic (Apache License 2.0)",
            "Rajdhani by Indian Type Foundry (SIL Open Font License)",
            "",
            "@LICENSE",
            "All rights reserved, every version (see LICENSE)",
            "Play it, record it and make mods for it",
            "No forks, copies or rebrands: contribute through issues",
            "",
            "@THE CAST",
            "Hype - the hype man with the mic",
            "Principal P - no rapping in the halls",
            "DJ Hand - the hand on the decks",
            "D-Mize - king of the underground",
            "",
            "Thank you for playing.",
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
std::unique_ptr<State> makeIntro() { return std::make_unique<IntroState>(); }
bool introWanted() { return app().settings.introVideo && FileExists(introPath().c_str()); }
std::unique_ptr<State> makeMainMenu() { return std::make_unique<MainMenuState>(); }
std::unique_ptr<State> makeStory() { return std::make_unique<StoryState>(); }
std::unique_ptr<State> makeFreeplay() { return std::make_unique<FreeplayState>(); }
std::unique_ptr<State> makeCredits() { return std::make_unique<CreditsState>(); }

}  // namespace gh
