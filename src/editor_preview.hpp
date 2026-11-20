// God's Hand of Principality and Demise. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// Live full-visuals preview for the chart editor: loads the song's stage,
// characters, Lua/HScript scripts and chart events into an offscreen
// scene::Scene and renders both the stage and the scrolling HUD notes
// (supporting upscroll and downscroll) into a RenderTexture2D.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "characters.hpp"
#include "keys.hpp"
#include "lua_host.hpp"
#include "psych.hpp"
#include "raylib.h"
#include "scene.hpp"
#include "scene_draw.hpp"
#include "song_scan.hpp"

namespace gh {

struct PreviewNote {
    double t = 0;
    double len = 0;
    uint8_t col = 0;
    bool mine = false;
    std::string type;
};

struct PreviewSection {
    bool mustHit = true, altAnim = false, gf = false;
    double bpm = 120, beats = 4;
    double startMs = 0, beatStart = 0;
};

struct PreviewConfig {
    const SongEntry* entry = nullptr;
    std::string diff, title, player, opponent, gf, stage;
    int keys = 4;
    double speed = 1.0;
    double lengthMs = 0;
    bool downscroll = false;
};

class ChartPreview : public ScriptGame {
public:
    ChartPreview();
    ~ChartPreview() override;
    ChartPreview(const ChartPreview&) = delete;
    ChartPreview& operator=(const ChartPreview&) = delete;

    void load(const PreviewConfig& cfg, const std::vector<PreviewSection>& sections,
              const std::vector<psych::ChartEvent>& events);
    void unload();
    bool loaded() const { return loaded_; }
    const std::string& status() const { return status_; }
    size_t scriptCount() const { return scripts_ ? scripts_->count() : 0; }

    void setDownscroll(bool ds);
    void setKeys(int keys);

    // Advances scripts, events, character singing/dancing and camera state to `curMs`.
    void sync(double curMs, double dt, bool playing, double speed, int keys,
              const std::vector<PreviewSection>& sections, const std::vector<PreviewNote>& notes,
              const std::vector<psych::ChartEvent>& events);

    // Renders the scene + HUD notes into the offscreen texture. Call outside gfx::beginCanvas().
    void render(double curMs, double speed, int keys, const std::vector<PreviewNote>& notes);

    // Draws the rendered preview texture into `dest` inside gfx::beginCanvas().
    void drawTo(Rectangle dest) const;

    // ScriptGame implementation
    double songPosition() const override { return curMs_; }
    bool getGameProp(std::string_view path, scene::Value& out) override;
    bool setGameProp(std::string_view path, const scene::Value& v) override;
    void triggerEvent(const std::string& name, const std::string& v1, const std::string& v2) override;
    void debugPrint(const std::string& text, uint32_t argb) override;
    void debugNotice(const std::string& text, bool error, bool debugMode) override;
    std::vector<std::string> singAnimations() override;
    void changeCharacter(int who, const std::string& name) override;
    bool shadersEnabled() override;

private:
    scene::CharacterPtr makeCharacter(const std::string& name, bool isPlayer, int slot);
    void placeCharacter(scene::Character& c, int slot);
    void setupStrums(int keys);
    void setGlobals(const std::vector<PreviewSection>& sections);
    void loadScripts(const std::vector<psych::ChartEvent>& events);
    void resetTimeline(double curMs, const std::vector<PreviewSection>& sections,
                       const std::vector<psych::ChartEvent>& events);
    void handleBuiltInEvent(const std::string& name, const std::string& v1, const std::string& v2);
    void onBeat(int beat, double curMs);
    void onStep(int step);
    void onSection(int secIdx, const PreviewSection& sec);
    void hitPreviewNote(const PreviewNote& n, double stepMs, const PreviewSection& sec);
    void drawHudNotes(double curMs, double speed, int keys, const std::vector<PreviewNote>& notes) const;
    void drawBuiltinChar(const scene::Character& c, Vector2 feet, float scale) const;

    PreviewConfig cfg_{};
    std::unique_ptr<scene::Scene> scene_;
    std::unique_ptr<ScriptHost> scripts_;
    SceneRenderer renderer_;
    RenderTexture2D rt_{};
    bool loaded_ = false;
    bool baseStage_ = false;
    std::string stageName_;
    std::string status_;

    double curMs_ = 0;
    double lastMs_ = -1;
    double speed_ = 1.0;
    double scrollSpeedMult_ = 1.0;
    double scrollSpeedFrom_ = 1.0, scrollSpeedTo_ = 1.0, scrollSpeedDur_ = 0, scrollSpeedT_ = 0;
    double curBpm_ = 120.0;
    double curStepMs_ = 125.0;
    int lastBeat_ = -1;
    int lastStep_ = -1;
    int lastSection_ = -1;
    size_t nextEvent_ = 0;

    // Camera bop / twist state
    int camBopRate_ = 4;
    double camBopMult_ = 1.0;
    bool camBopEnabled_ = true;
    double camTwistIntensity_ = 0, camTwistIntensity2_ = 0;
    double camTwistPhase_ = 0;

    // Strum hit animation timers (per column 0..2*kMaxKeys-1)
    struct StrumAnim {
        double startedAt = -10.0;
        double resetTimer = 0.0;
    };
    std::array<StrumAnim, 2 * kMaxKeys> strumAnim_{};
    double animNow_ = 0.0;

    // Built-in character fallback animation states (0 = bf, 1 = dad, 2 = gf)
    CharacterState builtinChars_[3]{};
};

}  // namespace gh
