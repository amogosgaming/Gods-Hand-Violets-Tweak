// God's Hand of Principality and Demise. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// The "Psych Engine" interface style: notes, receptors, holds, ratings,
// combo numbers, countdown, health bar, time bar, font, alphabet, checkboxes,
// title/story/main menu sprites, sounds and menu music.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "raylib.h"

namespace gh {

namespace scene {
class Files;
}

class PsychUi {
public:
    // Looks for the assets under `folder` (the engine's folder, its assets/
    // folder, or shared/). Returns how many pieces were found; `report`
    // says what is missing.
    int load(const std::string& folder, std::string& report);
    // The same, looking in `firstRoots` before the folder: a song's own mod
    // brings its own fonts, health bar, notes, ratings and sounds, as in Psych
    // (Paths looks in the current mod first).
    int load(const std::string& folder, std::string& report, const std::vector<std::string>& firstRoots);
    void unload();
    // Before load(): Psych's "Note Skins" option (images/noteSkins/NOTE_assets-<skin>.png,
    // "" = NOTE_assets) and "Note Colors" (per direction: main, highlight, outline).
    void setNoteSkin(const std::string& skin) {
        skin_ = skin;
        ++generation_;
    }
    // Once loaded: another note skin, from the same folders, without loading
    // the rest again (the Note skin screen's preview). False when it is off
    // or no NOTE_assets was found.
    bool changeNoteSkin(const std::string& skin);
    // Where else a note skin may be: every installed mod's folders.
    void setSkinRoots(std::vector<std::string> roots) { skinRoots_ = std::move(roots); }
    void setPalette(const uint32_t colors[4][3]);
    bool active() const { return active_; }
    // Changes whenever the notes may look different (loaded, skin, colours,
    // smoothing): pictures of notes made before are out of date.
    uint32_t generation() const { return generation_; }
    // Psych's antialiasing option: smooth (bilinear) or sharp scaling of the pictures.
    void setSmooth(bool smooth);
    void setActive(bool on) { active_ = on; }

    // Drawing in canvas units. `size` is a note's width, as in gfx::drawNote.
    // Each returns false when that piece is missing (draw the game's own).
    // Psych 0.7 / JS Engine note skins are colour templates: the notes of a
    // direction are drawn between beginDir and endDir to get their colours.
    void beginDir(int dir) const;
    void endDir() const;
    bool note(int dir, Vector2 c, float size, float alpha, float angle) const;
    // The width and height note() covers around its centre ({0, 0} without the skin).
    Vector2 noteExtent(int dir, float size) const;
    bool strum(int dir, Vector2 c, float size, int state, float alpha, float angle, double t) const;  // 0 idle, 1 pressed, 2 hit
    bool hold(int dir, float x, float y0, float y1, float width, float alpha) const;
    bool rating(int which, Vector2 c, float scale, float alpha) const;  // 0 sick .. 3 shit
    bool combo(long long value, Vector2 c, float scale, float alpha) const;
    // The rating and digit pictures with their top-left corner at (x, y), as
    // Psych's popUpScore places them; their sizes ({0, 0} when missing).
    Vector2 ratingSize(int which) const;
    Vector2 numSize(int digit) const;
    bool ratingAt(int which, float x, float y, float scale, float alpha) const;
    bool numAt(int digit, float x, float y, float scale, float alpha) const;
    bool countdown(int i, Vector2 c, float alpha) const;  // 0 ready, 1 set, 2 go
    // healthBar.png drawn at `frame` (its own size: healthBarSize(), {0, 0}
    // without it). Psych 0.7's picture is a frame drawn over the colours, 3
    // pixels in; 0.6's is a background under them, 4 pixels in (barInset()).
    Vector2 healthBarSize() const;
    float barInset() const { return barOnTop_ ? 3.0f : 4.0f; }
    bool barOnTop() const { return barOnTop_; }
    // frameAlpha: the picture's own (Psych 0.6 scripts fade healthBarBG on its own); < 0: alpha.
    bool healthBar(Rectangle frame, Color left, Color right, float leftFrac, float alpha, float frameAlpha = -1) const;
    // Psych's time bar (timeBar.png around a black bar and its white fill)
    // at `frame` (timeBarSize()); false when the interface is off.
    Vector2 timeBarSize() const;
    bool timeBar(Rectangle frame, float frac, float alpha, float frameAlpha = -1, Color fill = WHITE, Color back = BLACK) const;
    // HUD text in vcr.ttf with Psych's black outline, placed as a FlxText
    // whose box is at (x, y): centred on x when align is 0 (left edge at x:
    // -1, right edge: 1), `size` as FlxText's (Flash's em size, not raylib's
    // line height). False without the font.
    bool text(const std::string& s, float x, float y, float size, Color c, int align = 0, float border = 1.25f) const;
    // Always draws text: vcr.ttf when available, else the engine UI font with a black outline.
    void uiText(const std::string& s, float x, float y, float size, Color c, int align = 0, float border = 1.25f) const;
    float textWidth(const std::string& s, float size) const;  // as text() draws it
    float lineHeight(float size) const;                       // a FlxText line (OpenFL's ascent + descent)

    // Psych Engine Alphabet (images/alphabet.png + alphabet.xml), animated at 24fps.
    // `bold = true`: bold letters (menus, pause, headers, values);
    // `bold = false`: normal uppercase/lowercase letters (options labels).
    // `height` is the height of a bold capital 'A'. Returns width drawn, or -1 if missing.
    float alphabet(const std::string& s, float x, float y, float height, float alpha, bool bold = true, Color tint = WHITE) const;
    float alphabetWidth(const std::string& s, float height, bool bold = true) const;
    float alphabetCentered(const std::string& s, float cx, float y, float height, float alpha, bool bold = true, Color tint = WHITE) const;
    bool hasAlphabet() const { return alphabet_.id != 0; }

    // Psych Engine CheckboxThingie (images/checkboxanim.png/.xml).
    // `changeElapsed` < 0 draws the static checked/unchecked state; >= 0 plays the toggle animation.
    bool hasCheckbox() const { return checkbox_.tex.id != 0; }
    bool drawCheckbox(Vector2 pos, bool checked, double changeElapsed, float scale = 0.75f, float alpha = 1.0f) const;

    // Psych menu backgrounds: 0 menuBG (yellow), 1 menuDesat (tinted),
    // 2 menuBGBlue, 3 menuBGMagenta. Falls back across variants if one is missing.
    bool hasMenuBg() const { return menuBg_.id != 0 || menuDesat_.id != 0 || menuBgBlue_.id != 0 || menuBgMagenta_.id != 0; }
    bool drawMenuBg(int variant, Color tint, float scrollY = 0.0f, float scale = 1.1f) const;
    // Psych main menu button sprites (0 story_mode, 1 freeplay, 2 options, 3 credits).
    bool drawMainMenuItem(int which, Vector2 center, bool selected, double now, float alpha = 1.0f) const;

    // Psych Title Screen assets (logoBumpin, gfDanceTitle, titleEnter, newgrounds_logo, introText.txt).
    bool hasTitleAssets() const { return titleLogo_.tex.id != 0 || titleGf_.tex.id != 0; }
    double titleBpm() const { return titleBpm_; }
    bool drawTitleLogo(double beatElapsed, float alpha = 1.0f) const;
    bool drawTitleGf(bool danceRight, double beatElapsed, float alpha = 1.0f) const;
    bool drawTitleEnter(bool pressed, bool flashing, double now, double pressedAt, float alpha = 1.0f) const;
    bool drawNewgroundsLogo(Vector2 center, float scale = 0.8f, float alpha = 1.0f) const;
    const std::vector<std::pair<std::string, std::string>>& introTexts() const { return introTexts_; }

    // Psych Story Mode assets (Menu_Tracks, storymenu/*, menudifficulties/*, campaign_menu_UI_assets,
    // menubackgrounds/menu_stage, menucharacters/Menu_{Dad,BF,GF}).
    bool drawTracksHeader(Vector2 pos, float alpha = 1.0f) const;
    bool drawStoryWeekTitle(int weekIdx, Vector2 center, float alpha = 1.0f, Color tint = WHITE) const;
    bool drawDifficulty(int diffIdx, Vector2 center, float alpha = 1.0f, float scale = 1.0f) const;
    bool drawCampaignArrow(bool right, bool pushed, Vector2 pos, float alpha = 1.0f) const;
    bool drawStoryStageBg(Rectangle rect, float alpha = 1.0f) const;
    // which: 0 = Dad, 1 = BF, 2 = GF.
    bool drawStoryChar(int which, Vector2 pos, double beatElapsed, bool hey = false, double heyElapsed = 0.0, float alpha = 1.0f) const;

    const Font* font() const { return font_.texture.id ? &font_ : nullptr; }
    const std::string& menuMusic() const { return menuMusic_; }
    // The pause menu's song: 0 none, 1 breakfast, 2 tea-time (the other one
    // when the chosen one is missing).
    std::string pauseMusic(int which) const;
    void applySounds() const;  // menu and gameplay sounds replace the game's own

private:
    struct Frame {
        Rectangle src{};
        float fx = 0, fy = 0, fw = 0, fh = 0;
        bool rotated = false;
    };
    struct Glyph {
        std::vector<Frame> frames;
        float offX = 0, offY = 0;  // Psych AlphaCharacter.allLetters offset (positive offY moves UP)
    };
    struct Sheet {
        Texture2D tex{};
        std::vector<Frame> frames;
    };
    bool drawFrame(const Frame& f, Vector2 c, float scale, float alpha, float angle, bool flipY = false) const;
    static void drawSheetFrameTopLeft(const Texture2D& tex, const Frame& f, Vector2 pos, float scale, Color tint);
    static void drawSheetFrameCentered(const Texture2D& tex, const Frame& f, Vector2 center, float scale, Color tint);
    bool loadNotes(const scene::Files& f);
    void unloadNotes();
    std::vector<Frame> frames(const std::string& prefix) const;

    bool active_ = false;
    uint32_t generation_ = 0;
    std::string root_, skin_;
    std::vector<std::string> skinRoots_;
    std::vector<std::string> roots_;  // where load() looked (changeNoteSkin looks there again)
    uint32_t palette_[4][3] = {{0xC24B99, 0xFFFFFF, 0x3C1F56},
                               {0x00FFFF, 0xFFFFFF, 0x1542B7},
                               {0x12FA05, 0xFFFFFF, 0x0A4447},
                               {0xF9393F, 0xFFFFFF, 0x651038}};
    bool smooth_ = true;
    Texture2D notes_{};
    bool template_ = false;  // RGB template skin (recoloured per direction)
    Shader paletteShader_{};
    int locR_ = -1, locG_ = -1, locB_ = -1;
    std::vector<std::pair<std::string, Frame>> atlas_;  // NOTE_assets frames by name
    Texture2D ratings_[4]{}, nums_[10]{}, count_[3]{}, bar_{}, timeBar_{};
    bool barOnTop_ = true, timeOnTop_ = true;  // Psych 0.7's frames (see barInset())
    Font font_{};
    // raylib sizes a font by its line height (ascent - descent), Flash by its
    // em square: a FlxText of size 20 is drawn at 20 * fontScale_.
    float fontScale_ = 1.0f, fontAscent_ = 1.0f;
    Texture2D alphabet_{};
    std::vector<std::pair<std::string, Frame>> letters_;  // alphabet frames ("a bold instance 10000"...)
    Texture2D menuBg_{}, menuDesat_{}, menuBgBlue_{}, menuBgMagenta_{};
    struct MenuSheet {
        Texture2D tex{};
        std::vector<Frame> basic, white;
    };
    MenuSheet menuItems_[4]{};  // story_mode, freeplay, options, credits

    // CheckboxThingie
    struct CheckboxSheet {
        Texture2D tex{};
        std::vector<Frame> unchecked, anim, animRev, checked;
    };
    CheckboxSheet checkbox_{};

    // Title screen
    Sheet titleLogo_{};
    Sheet titleGf_{};
    struct TitleEnterSheet {
        Texture2D tex{};
        std::vector<Frame> idle, pressed, freeze;
    };
    TitleEnterSheet titleEnter_{};
    Texture2D newgroundsLogo_{};
    float titleLogoX_ = -150.0f, titleLogoY_ = -100.0f;
    float titleGfX_ = 512.0f, titleGfY_ = 40.0f;
    float titleStartX_ = 100.0f, titleStartY_ = 576.0f;
    double titleBpm_ = 102.0;
    std::vector<std::pair<std::string, std::string>> introTexts_;

    // Story mode
    Texture2D menuTracks_{};
    Texture2D storyWeeks_[2]{};   // tutorial, week1
    Texture2D storyDiffs_[3]{};   // easy, normal, hard
    Texture2D storyStageBg_{};
    struct CampaignUiSheet {
        Texture2D tex{};
        std::vector<Frame> arrowLeft, pushLeft, arrowRight, pushRight, lock;
    };
    CampaignUiSheet campaignUi_{};
    struct StoryCharSheet {
        Texture2D tex{};
        std::vector<Frame> idle, confirm;
        float offX = 0.0f, offY = 0.0f, scale = 1.0f;
    };
    StoryCharSheet storyChars_[3]{};  // 0 dad, 1 bf, 2 gf

    const Glyph* glyph(char c, bool bold) const;
    Glyph findGlyph(char c, bool bold) const;
    mutable std::array<Glyph, 128> boldGlyphs_{};
    mutable std::array<bool, 128> boldKnown_{};
    mutable std::array<Glyph, 128> normalGlyphs_{};
    mutable std::array<bool, 128> normalKnown_{};

    std::vector<std::pair<int, std::string>> sounds_;  // Sfx id -> file
    std::string menuMusic_, pauseMusic_, teaTime_;
};

PsychUi& psychUi();

// Psych Engine / JS Engine copies already on the player's disk: engine builds
// installed as mod packs (mods/), or unpacked in or next to the game's folder.
// The most complete first (an engine's own build before a mod's reskin).
// The game uses the best one when no folder was chosen.
struct PsychFolder {
    std::string path;
    int score = 0;  // pieces of the interface found
};
std::vector<PsychFolder> findPsychFolders(const std::string& gameBase);
int psychFolderScore(const std::string& folder);
// Whether a mod's folders hold any piece of the interface (a font, the health
// bar, a note skin, ratings, sounds...).
bool hasPsychPieces(const std::vector<std::string>& roots);
// The note skins a copy has (images/noteSkins/NOTE_assets-<name>.png), by name.
// `modRoots`: every installed mod's folders too (their noteSkins/ count).
std::vector<std::string> psychNoteSkins(const std::string& folder, const std::vector<std::string>& modRoots = {});

}  // namespace gh
