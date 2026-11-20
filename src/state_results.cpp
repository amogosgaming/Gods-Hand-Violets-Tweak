// God's Hand of Principality and Demise. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// End-of-song results.
#include <cmath>

#include "app.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "psych_ui.hpp"
#include "raylib.h"

namespace gh {

using gfx::Align;
using gfx::FontId;
using gfx::fade;

namespace {
// Text that shrinks (centred on its line) to fit maxW pixels: big numbers from spam charts.
void fitText(FontId f, const std::string& s, float x, float y, float size, float maxW, Color c, Align a, float outline) {
    const float w = gfx::measure(f, s, size).x;
    if (w > maxW) {
        const float ns = size * maxW / w;
        y += (size - ns) * 0.5f;
        size = ns;
    }
    gfx::text(f, s, x, y, size, c, a, outline);
}
const Color kGold{255, 222, 64, 255};   // the accent: a bright, cartoon yellow
const Color kIvory{242, 244, 255, 255};  // text: a cool white

Color rankColor(const std::string& r) {
    if (r == "S+" || r == "S") return Color{255, 214, 100, 255};
    if (r == "A") return Color{120, 230, 150, 255};
    if (r == "B") return Color{120, 190, 255, 255};
    if (r == "C") return Color{190, 150, 255, 255};
    return Color{255, 90, 100, 255};
}
const Color kRatingCols[4] = {{255, 215, 100, 255}, {120, 220, 255, 255}, {205, 205, 220, 255}, {205, 120, 150, 255}};
}  // namespace

class ResultsState : public State {
public:
    explicit ResultsState(ResultData r) : r_(std::move(r)) {}
    TouchLayout touchLayout() const override {
        TouchLayout t;
        if (!r_.replayPath.empty()) t.keys = {{"WATCH REPLAY", KEY_R}};
        return t;
    }
    void enter() override {
        t0_ = app().now;
        sfxPlay(r_.rank == "S" || r_.rank == "S+" ? Sfx::Unlock : Sfx::Confirm);
        double sum = 0, sq = 0;
        for (const auto& e : r_.errors) { sum += e.err; sq += double(e.err) * e.err; }
        const double n = double(std::max<size_t>(1, r_.errors.size()));
        mean_ = sum / n;
        sd_ = std::sqrt(std::max(0.0, sq / n - mean_ * mean_));
    }
    void update(double) override {
        App& a = app();
        if (a.autoRun.enabled && a.autoRun.quitAfter < 0 && a.now - t0_ > 2.5) a.quit = true;
        if (a.now - t0_ < 0.6) return;
        if (!r_.replayPath.empty() && a.input.pressed(KEY_R)) {
            sfxPlay(Sfx::Confirm);
            PlayRequest n;
            n.songId = r_.req.songId;
            n.diff = r_.req.diff;
            n.replayPath = r_.replayPath;
            a.change(makePlay(n));
            return;
        }
        if (a.input.accept() || a.input.back()) {
            sfxPlay(Sfx::Confirm);
            if (r_.req.story && !r_.req.storyQueue.empty()) {
                PlayRequest n = r_.req;
                n.songId = n.storyQueue.front();
                n.storyQueue.erase(n.storyQueue.begin());
                a.change(makePlay(n));
            } else {
                a.change(r_.req.story ? makeStory() : makeFreeplay());
            }
        }
    }
    void draw() override {
        App& a = app();
        const double now = a.now;
        const float t = float(now - t0_);
        const bool psychStyle = a.psychMenusActive();
        const PsychUi& ui = psychUi();
        gfx::beginCanvas();
        if (psychStyle) {
            drawPsychMenuBackground(1, gfx::rgb(r_.color), 0.0f, false, now, r_.color, menuPulse());
            DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.45f));
        } else {
            drawMenuBackground(now, r_.color, menuPulse());
        }
        auto drawTxt = [&](const std::string& s, float x, float y, float size, Color c, int align, float outline) {
            if (psychStyle) ui.uiText(s, x, y, size * 0.85f, c, align, outline);
            else gfx::text(FontId::Ui, s, x, y, size, c, align < 0 ? Align::Left : (align > 0 ? Align::Right : Align::Center), outline);
        };
        // rank
        const float pop = easeOutBack(saturate(t * 2.0f));
        const Color rc = rankColor(r_.rank);
        if (psychStyle) {
            if (ui.alphabetCentered(r_.rank, 250, 150 + (1 - pop) * 50, 140 * std::max(0.2f, pop), saturate(t * 3)) < 0)
                ui.uiText(r_.rank, 250, 140 + (1 - pop) * 50, 140 * std::max(0.2f, pop), fade(rc, saturate(t * 3)), 0, 3.0f);
        } else {
            gfx::glowText(FontId::Title, r_.rank, 250, 120 + (1 - pop) * 60, 210 * std::max(0.2f, pop), fade(rc, saturate(t * 3)), rc);
        }
        if (!r_.clear.empty()) {
            if (psychStyle) ui.uiText(r_.clear, 250, 380, 30, kGold, 0, 2.0f);
            else gfx::text(FontId::Title, r_.clear, 250, 380, 34, kGold, Align::Center, 2.0f);
        }
        if (r_.newBest && std::fmod(t * 2.0f, 2.0f) < 1.5f)
            drawTxt("NEW BEST!", 250, 430, 36, WHITE, 0, 2.0f);
        if (r_.botplay) drawTxt("Botplay - not saved", 250, 470, 24, fade(kIvory, 0.7f), 0, 1.0f);
        else if (r_.practice) drawTxt("Practice - not saved", 250, 470, 24, fade(kIvory, 0.7f), 0, 1.0f);
        // stats
        const float x = 520;
        if (psychStyle) {
            if (ui.alphabet(r_.title, x, 46, 46, 1.0f) < 0) ui.uiText(r_.title, x, 50, 38, WHITE, -1, 2.0f);
        } else {
            gfx::text(FontId::Title, r_.title, x, 50, 44, kGold, Align::Left, 2.0f);
        }
        drawTxt(r_.diff + (r_.rate != 1.0 ? std::string(TextFormat("   %.2fx", r_.rate)) : std::string()), x, 104, 28,
                kIvory, -1, 1.5f);
        const float shown = saturate(t * 1.5f);
        drawTxt("SCORE", x, 150, 22, fade(kGold, 0.85f), -1, 1.0f);
        const std::string scoreStr = fmtThousands(shown >= 1 ? r_.score : (long long)(double(r_.score) * shown));
        if (psychStyle) ui.uiText(scoreStr, x, 174, 44, WHITE, -1, 2.0f);
        else fitText(FontId::Ui, scoreStr, x, 170, 56, 360, WHITE, Align::Left, 2.0f);
        drawTxt("ACCURACY", x + 380, 150, 22, fade(kGold, 0.85f), -1, 1.0f);
        drawTxt(TextFormat("%.2f%%", r_.acc * 100.0 * shown), x + 380, 174, 52, WHITE, -1, 2.0f);
        static const char* names[4] = {"SICK!", "GOOD", "BAD", "WACK"};
        for (int i = 0; i < 4; ++i) {
            drawTxt(names[i], x, 250.0f + i * 38, 30, kRatingCols[i], -1, 1.5f);
            if (psychStyle) ui.uiText(fmtThousands(r_.counts[size_t(i)]), x + 350, 250.0f + i * 38, 26, WHITE, 1, 1.5f);
            else fitText(FontId::Ui, fmtThousands(r_.counts[size_t(i)]), x + 350, 250.0f + i * 38, 30, 350 - 130, WHITE, Align::Right, 1.5f);
        }
        drawTxt("MISSES", x + 380, 250, 30, Color{255, 90, 100, 255}, -1, 1.5f);
        if (psychStyle) ui.uiText(fmtThousands(r_.misses), x + 640, 250, 26, WHITE, 1, 1.5f);
        else fitText(FontId::Ui, fmtThousands(r_.misses), x + 640, 250, 30, 260 - 110, WHITE, Align::Right, 1.5f);
        drawTxt("MAX COMBO", x + 380, 288, 30, kGold, -1, 1.5f);
        if (psychStyle) ui.uiText(fmtThousands(r_.maxCombo), x + 640, 288, 26, WHITE, 1, 1.5f);
        else fitText(FontId::Ui, fmtThousands(r_.maxCombo), x + 640, 288, 30, 260 - 160, WHITE, Align::Right, 1.5f);
        drawTxt("TIMING", x + 380, 326, 30, kIvory, -1, 1.5f);
        drawTxt(TextFormat("%+.1f / %.1f ms", mean_, sd_), x + 640, 326, 30, WHITE, 1, 1.5f);
        // Notes hit / notes in the chart; "Both sides" (Options, Note counter) adds the opponent's.
        long long hit = 0, total = r_.totalNotes;
        for (long long c : r_.counts) hit += c;
        const bool both = app().settings.noteCounter == 2;
        if (both) {
            hit += r_.opponentHits;
            total += r_.opponentNotes;
        }
        drawTxt(both ? "NOTES (BOTH)" : "NOTES", x + 380, 364, both ? 22 : 30, kIvory, -1, 1.5f);
        if (psychStyle) ui.uiText(fmtThousands(hit) + " / " + fmtThousands(total), x + 640, 364, 24, WHITE, 1, 1.5f);
        else fitText(FontId::Ui, fmtThousands(hit) + " / " + fmtThousands(total), x + 640, 364, 30, 260 - (both ? 125 : 95), WHITE,
                     Align::Right, 1.5f);
        // timing graph
        const Rectangle g{x, 420, 680, 190};
        DrawRectangleRounded({g.x - 8, g.y - 8, g.width + 16, g.height + 16}, 0.06f, 8, Color{8, 4, 14, 200});
        const float cy = g.y + g.height * 0.5f;
        for (int k = -3; k <= 3; ++k) {
            const float yy = cy - float(k) * 45.0f / 166.0f * g.height * 0.5f;
            DrawLineEx({g.x, yy}, {g.x + g.width, yy}, k == 0 ? 2.0f : 1.0f, fade(WHITE, k == 0 ? 0.35f : 0.12f));
        }
        const size_t n = r_.errors.size();
        const size_t stride = std::max<size_t>(1, n / 5000);
        const size_t upto = size_t(double(n) * saturate(t * 1.2f));
        for (size_t i = 0; i < upto; i += stride) {
            const auto& e = r_.errors[i];
            const float px = g.x + float(e.t / r_.lengthMs) * g.width;
            const float py = cy + clampv(e.err, -166.0f, 166.0f) / 166.0f * g.height * 0.5f;
            const float ae = std::fabs(e.err);
            const int ri = ae <= 45 ? 0 : ae <= 90 ? 1 : ae <= 135 ? 2 : 3;
            DrawCircleV({px, py}, 2.2f, kRatingCols[ri]);
        }
        drawTxt("early", g.x + 6, g.y + 2, 18, fade(kIvory, 0.6f), -1, 1.0f);
        drawTxt("late", g.x + 6, g.y + g.height - 20, 18, fade(kIvory, 0.6f), -1, 1.0f);
        if (r_.req.story && r_.req.storyQueue.empty())
            drawTxt("WEEK COMPLETE  -  " + fmtThousands(r_.req.storyScore), 250, 540, 28, kGold, 0, 2.0f);
        DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, static_cast<unsigned char>(psychStyle ? 185 : 150)});
        const bool next = r_.req.story && !r_.req.storyQueue.empty();
        const char* ok = a.touchMode ? "OK" : "ENTER";
        std::string hint = std::string(ok) + (next ? " next song" : " continue");
        if (!r_.replayPath.empty() && !a.touchMode) hint += "      R watch replay";
        if (r_.replay) hint = "REPLAY      " + hint;
        if (psychStyle) ui.uiText(hint, gfx::W / 2, gfx::H - 31, 18, WHITE, 0, 1.0f);
        else gfx::text(FontId::Ui, hint, gfx::W / 2, gfx::H - 33, 22, Color{220, 214, 235, 255}, Align::Center);
        gfx::endCanvas();
    }

private:
    ResultData r_;
    double t0_ = 0, mean_ = 0, sd_ = 0;
};

std::unique_ptr<State> makeResults(const ResultData& r) { return std::make_unique<ResultsState>(r); }

}  // namespace gh
