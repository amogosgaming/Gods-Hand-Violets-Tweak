// God's Hand of Principality and Demise. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// The editors' menu (main menu: 7), as Psych Engine's Master Editor Menu: the
// chart editor and the character, stage and event editors. They work on the
// mods the game found; the menu picks the mod they save into.
//
// Character editor: a Psych character JSON (characters/<name>.json): its
// animations and their offsets (the arrow keys, with the idle ghost behind),
// image, scale, flip, health icon and bar colour, position and camera point.
// Stage editor: a Psych stage JSON (stages/<name>.json): zoom, where the three
// characters stand and where the camera looks at each, shown over the stage
// itself (its JSON objects and its Lua script) with the characters dragged by
// the mouse. Event editor: custom events (custom_events/<name>.txt and .lua)
// from a template. Files keep the fields the editors do not know.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <mutex>
#include <cstring>

#include "app.hpp"
#include "basegame.hpp"
#include "chart_binary.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "editor_ui.hpp"
#include "flp_import.hpp"
#include "gfx.hpp"
#include "json.hpp"
#include "keys.hpp"
#include "lua_host.hpp"
#include "process.hpp"
#include "psych.hpp"
#include "psych_ui.hpp"
#include "raylib.h"
#include "scene.hpp"
#include "scene_draw.hpp"

namespace gh {

namespace {
namespace fs = std::filesystem;
using namespace edui;
using gfx::fade;

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

const Color kGold{255, 222, 64, 255};   // the accent: a bright, cartoon yellow
const Color kIvory{242, 244, 255, 255};  // text: a cool white
constexpr float kPanelX = 872, kPanelW = gfx::W - kPanelX - 8;

// The keys line at the bottom, as the menus show it.
void hints(const char* s) {
    if (app().touchMode) return;
    DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 150});
    gfx::text(FontId::Ui, s, gfx::W / 2, gfx::H - 33, 20, Color{220, 214, 235, 255}, Align::Center);
}

// ------------------------------------------------------------------- mods --

struct ModChoice {
    std::string name;                // its folder in mods/ ("" = the mods folder itself)
    std::vector<std::string> roots;  // its asset folders; the editors save into roots[0]
};

std::vector<ModChoice> modChoices() {
    App& a = app();
    std::vector<ModChoice> out;
    for (const auto& [pack, roots] : a.library.packRoots())
        if (!pack.empty() && !roots.empty()) out.push_back({pack, roots});
    out.push_back({"", {a.base + "mods/"}});
    return out;
}

int& modIndex() {
    static int i = 0;
    return i;
}

ModChoice currentMod() {
    const std::vector<ModChoice> list = modChoices();
    int& i = modIndex();
    i = clampv(i, 0, int(list.size()) - 1);
    return list[size_t(i)];
}

std::string modLabel(const ModChoice& m) { return m.name.empty() ? std::string("mods/ (no mod)") : m.name; }

void useMod(scene::Scene& sc, const ModChoice& m) {
    std::vector<std::string> roots = m.roots;
    roots.push_back(app().base + "mods/");
    sc.files.setRoots(roots);
    sc.files.setSharedRoots(app().library.allAssetRoots());
}

// The files in `dir` ending in `ext`, by name: the chosen mod's first, then
// the ones other mods have.
std::vector<std::string> listNames(const scene::Scene& sc, const std::string& dir, const std::string& ext) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto scan = [&](const std::vector<std::string>& roots) {
        std::vector<std::string> found;
        for (const std::string& r : roots) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(toPath(r + dir), ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string n = pathStr(e.path().filename());
                if (!endsWithI(n, ext)) continue;
                n.resize(n.size() - ext.size());
                if (seen.insert(n).second) found.push_back(n);
            }
        }
        std::sort(found.begin(), found.end());
        out.insert(out.end(), found.begin(), found.end());
    };
    scan(sc.files.roots());
    scan(sc.files.sharedRoots());
    return out;
}

// ------------------------------------------------------------ JSON files --

std::string jnum(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string((long long)v);
    char b[48];
    std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// A JSON value as Psych's editors write it: tabs, short arrays on one line.
void emit(const json::Document& d, const json::Value& v, std::string& o, int ind) {
    const std::string pad(size_t(ind), '\t');
    switch (v.type) {
        case json::Type::Null: o += "null"; break;
        case json::Type::Bool: o += v.b ? "true" : "false"; break;
        case json::Type::Number: o += jnum(v.num); break;
        case json::Type::String: o += jsonStr(std::string(d.str(v))); break;
        case json::Type::Array: {
            const size_t n = d.size(v);
            bool flat = true;
            for (size_t i = 0; i < n; ++i) {
                const json::Type t = d.at(v, i).type;
                flat = flat && t != json::Type::Array && t != json::Type::Object;
            }
            if (n == 0 || flat) {
                o += "[";
                for (size_t i = 0; i < n; ++i) {
                    if (i) o += ", ";
                    emit(d, d.at(v, i), o, ind);
                }
                o += "]";
                break;
            }
            o += "[\n";
            for (size_t i = 0; i < n; ++i) {
                o += pad + "\t";
                emit(d, d.at(v, i), o, ind + 1);
                o += i + 1 < n ? ",\n" : "\n";
            }
            o += pad + "]";
            break;
        }
        case json::Type::Object: {
            const size_t n = d.size(v);
            if (n == 0) {
                o += "{}";
                break;
            }
            o += "{\n";
            for (size_t i = 0; i < n; ++i) {
                o += pad + "\t" + jsonStr(std::string(d.keyAt(v, i))) + ": ";
                emit(d, d.valAt(v, i), o, ind + 1);
                o += i + 1 < n ? ",\n" : "\n";
            }
            o += pad + "}";
            break;
        }
    }
}

// An object: `fields` (key, JSON text) in their order, then the fields of the
// file as it was (`oldText`) that `fields` does not set.
std::string writeObject(const std::vector<std::pair<std::string, std::string>>& fields, const std::string& oldText) {
    std::vector<std::string> parts;
    std::set<std::string> have;
    for (const auto& [k, v] : fields) {
        parts.push_back("\t" + jsonStr(k) + ": " + v);
        have.insert(k);
    }
    json::Document d;
    if (!oldText.empty() && d.parse(oldText) && d.root().type == json::Type::Object) {
        const json::Value& r = d.root();
        for (size_t i = 0; i < d.size(r); ++i) {
            const std::string k(d.keyAt(r, i));
            if (have.count(k)) continue;
            std::string v;
            emit(d, d.valAt(r, i), v, 1);
            parts.push_back("\t" + jsonStr(k) + ": " + v);
        }
    }
    std::string o = "{\n";
    for (size_t i = 0; i < parts.size(); ++i) o += parts[i] + (i + 1 < parts.size() ? ",\n" : "\n");
    return o + "}\n";
}

std::string pair(double a, double b) { return "[" + jnum(a) + ", " + jnum(b) + "]"; }

bool saveText(const std::string& path, const std::string& text, std::string& err) {
    std::error_code ec;
    fs::create_directories(toPath(path).parent_path(), ec);
    std::ofstream f(toPath(path), std::ios::binary);
    if (!f) {
        err = "could not write " + path;
        return false;
    }
    f << text;
    if (!f) err = "could not write " + path;
    return bool(f);
}

// A safe file name for what the player typed.
std::string fileName(std::string s) {
    s = trim(s);
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '-';
    return s;
}

// ------------------------------------------------------------- characters --

bool loadCharacterDef(const scene::Scene& sc, const std::string& name, psych::CharacterDef& def, std::string& path, std::string& text,
                      std::string& err) {
    def = psych::CharacterDef{};
    text.clear();
    path = sc.files.find("characters/" + name + ".json");
    if (path.empty()) path = sc.files.find("data/characters/" + name + ".json");
    if (!path.empty()) {
        if (!scene::readFile(path, text)) {
            err = "could not read " + path;
            return false;
        }
        return psych::parseCharacter(text, def, err);
    }
    if (const char* base = basegame::characterJson(name)) {  // older engines' hard-coded characters
        path.clear();
        return psych::parseCharacter(base, def, err);
    }
    err = "characters/" + name + ".json not found";
    return false;
}

std::string imageKeys(const psych::CharacterDef& def) {
    std::string keys;
    for (const std::string& im : def.images) keys += (keys.empty() ? "" : ",") + im;
    return keys;
}

// The character as the play state builds it (Psych's Character), or nullptr
// when its image cannot be found.
scene::CharacterPtr buildCharacter(scene::Scene& sc, const psych::CharacterDef& def, bool isPlayer, std::string& err) {
    const std::string keys = imageKeys(def);
    scene::GraphicPtr g = keys.empty() ? nullptr : sc.gfx.atlas(keys);
    if (!g) {
        err = "image not found: " + (keys.empty() ? std::string("(none)") : keys);
        return nullptr;
    }
    auto c = std::make_shared<scene::Character>();
    c->kind = scene::Kind::Character;
    c->def = def;
    c->isPlayer = isPlayer;
    c->setGraphic(g);
    for (const psych::AnimDef& ad : def.anims) {
        scene::Anim a;
        a.name = ad.anim;
        a.frames = ad.indices.empty() ? psych::framesByPrefix(g->frames, ad.prefix) : psych::framesByIndices(g->frames, ad.prefix, ad.indices);
        if (a.frames.empty()) continue;
        a.fps = ad.fps;
        a.loop = ad.loop;
        a.hasOffset = true;
        a.offX = ad.offX;
        a.offY = ad.offY;
        c->addAnim(std::move(a));
    }
    if (def.scale != 1.0) {
        c->setGraphicSize(std::floor(c->width * float(def.scale)), 0);
        c->updateHitbox();
    }
    c->flipX = def.flipX != isPlayer;
    c->antialiasing = def.antialiasing;
    c->camX = def.camX;
    c->camY = def.camY;
    c->posX = def.posX;
    c->posY = def.posY;
    c->icon = def.icon;
    c->barColor = def.barColor;
    c->singDuration = def.singDuration;
    if (!def.anims.empty()) c->playAnim(def.anims[0].anim, true);
    return c;
}

// The animation names an atlas holds (frame names without their numbers).
std::vector<std::string> atlasPrefixes(const scene::Graphic& g) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const psych::AtlasFrame& f : g.frames) {
        std::string n = f.name;
        while (!n.empty() && std::isdigit((unsigned char)n.back())) n.pop_back();
        if (!n.empty() && seen.insert(n).second) out.push_back(n);
    }
    return out;
}

std::string iconPath(const scene::Scene& sc, const std::string& icon) {
    for (const std::string& rel : {"images/icons/icon-" + icon + ".png", "images/icons/" + icon + ".png", "images/icon-" + icon + ".png"}) {
        const std::string p = sc.files.find(rel);
        if (!p.empty()) return p;
    }
    return {};
}

// Psych's "Get Icon Color": the commonest colour of the icon's first frame,
// black and see-through pixels left out.
bool iconColor(const std::string& path, uint32_t& rgb) {
    Image img = LoadImage(path.c_str());
    if (!img.data) return false;
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const int w = img.width >= img.height * 2 ? img.width / 2 : img.width;
    std::map<uint32_t, int> count;
    const unsigned char* px = static_cast<const unsigned char*>(img.data);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = px + (size_t(y) * size_t(img.width) + size_t(x)) * 4;
            if (p[3] == 0) continue;
            const uint32_t c = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
            if (c == 0) continue;
            ++count[c];
        }
    UnloadImage(img);
    int best = 0;
    for (const auto& [c, n] : count)
        if (n >= best) {
            best = n;
            rgb = c;
        }
    return best > 0;
}

// ------------------------------------------------------------- the view --

// What part of the world the editor shows (centre and zoom), through the
// game camera of its scene; dragged with the right mouse button, zoomed with
// the wheel (and two-finger pinch/pan or optional one-finger pan on touch).
struct View {
    float x = 640, y = 360, zoom = 1;
    bool dragging = false;
    bool touchPanning = false;
    bool pinching = false;
    Vector2 last{};
    Vector2 pinchMid{};
    float pinchDist = 0;

    void apply(scene::Camera& cam) const {
        cam.zoom = zoom;
        cam.scrollX = x - gfx::W * 0.5f;
        cam.scrollY = y - gfx::H * 0.5f;
    }
    Vector2 toWorld(Vector2 canvas) const { return {x + (canvas.x - gfx::W * 0.5f) / zoom, y + (canvas.y - gfx::H * 0.5f) / zoom}; }
    Vector2 toCanvas(float wx, float wy) const { return {gfx::W * 0.5f + (wx - x) * zoom, gfx::H * 0.5f + (wy - y) * zoom}; }
    void handle(const Input& in, const Ui& ui, bool overPanel, bool oneFingerPan = false) {
        const Vector2 m = ui.mouse;
        if (ui.touchMode) {
            const auto pts = touchPoints();
            if (pts.size() >= 2) {
                const Vector2 p0 = gfx::toCanvas(pts[0].pos), p1 = gfx::toCanvas(pts[1].pos);
                const Vector2 mid{(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
                const float dist = std::max(8.0f, std::hypot(p1.x - p0.x, p1.y - p0.y));
                if (!pinching) {
                    pinching = true;
                    touchPanning = false;
                    pinchMid = mid;
                    pinchDist = dist;
                } else {
                    const Vector2 before = toWorld(pinchMid);
                    zoom = clampv(zoom * (dist / pinchDist), 0.05f, 8.0f);
                    const Vector2 after = toWorld(mid);
                    x += before.x - after.x;
                    y += before.y - after.y;
                    pinchMid = mid;
                    pinchDist = dist;
                }
                return;
            }
            pinching = false;
            if (oneFingerPan) {
                if (!overPanel && ui.click && !ui.used) {
                    touchPanning = true;
                    last = m;
                }
                if (touchPanning) {
                    if (!ui.down) {
                        touchPanning = false;
                    } else {
                        x -= (m.x - last.x) / zoom;
                        y -= (m.y - last.y) / zoom;
                        last = m;
                    }
                }
            } else {
                touchPanning = false;
            }
        }
        if (!overPanel && (in.mousePressed(MOUSE_BUTTON_RIGHT) || in.mousePressed(MOUSE_BUTTON_MIDDLE))) {
            dragging = true;
            last = m;
        }
        if (dragging) {
            if (!IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && !IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) dragging = false;
            x -= (m.x - last.x) / zoom;
            y -= (m.y - last.y) / zoom;
            last = m;
        }
        if (!overPanel && ui.wheel != 0 && !ui.used) {
            const Vector2 before = toWorld(m);
            zoom = clampv(zoom * (ui.wheel > 0 ? 1.1f : 1.0f / 1.1f), 0.05f, 8.0f);
            const Vector2 after = toWorld(m);
            x += before.x - after.x;
            y += before.y - after.y;
        }
    }
};

void drawGrid(const View& v) {
    DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{24, 20, 34, 255});
    const float step = 100 * v.zoom;
    if (step < 8) return;
    const Vector2 o = v.toCanvas(0, 0);
    for (float x = std::fmod(o.x, step); x < gfx::W; x += step) DrawLineEx({x, 0}, {x, gfx::H}, 1, Color{40, 34, 56, 255});
    for (float y = std::fmod(o.y, step); y < gfx::H; y += step) DrawLineEx({0, y}, {gfx::W, y}, 1, Color{40, 34, 56, 255});
    DrawLineEx({o.x, 0}, {o.x, gfx::H}, 1.5f, Color{70, 60, 100, 255});
    DrawLineEx({0, o.y}, {gfx::W, o.y}, 1.5f, Color{70, 60, 100, 255});
}

void cross(Vector2 c, float r, Color col) {
    DrawLineEx({c.x - r, c.y}, {c.x + r, c.y}, 3, BLACK);
    DrawLineEx({c.x, c.y - r}, {c.x, c.y + r}, 3, BLACK);
    DrawLineEx({c.x - r, c.y}, {c.x + r, c.y}, 1.5f, col);
    DrawLineEx({c.x, c.y - r}, {c.x, c.y + r}, 1.5f, col);
}

void panelBox(float y0, float y1, float px = kPanelX) {
    const float pw = gfx::W - px - 8.0f;
    DrawRectangleRounded({px - 6, y0, pw + 12, y1 - y0}, 0.03f, 6, kPanel);
    DrawRectangleRoundedLinesEx({px - 6, y0, pw + 12, y1 - y0}, 0.03f, 6, 1.5f, Color{110, 92, 150, 160});
}

void feedUi(Ui& ui) {
    App& a = app();
    SceneRenderer::smoothScaling = a.settings.antialiasing;
    ui.syncPointer(a.input, gfx::toCanvas(GetMousePosition()), a.touchMode, a.touchMode && a.touch.covers(GetMousePosition()));
}

bool overPanel(const Ui& ui, bool panelOpen = true, float px = kPanelX) {
    return !ui.dropId.empty() || (panelOpen && ui.mouse.x >= px - 8);
}

// Scripts in the editors: a stage's Lua builds its sprites; nothing else happens.
struct EditorGame : ScriptGame {
    double clockMs = 0;     // the stage editor runs a clock so scripts and shaders move
    bool shaders = false;   // the stage editor shows shaders (as the Shaders option says)
    double songPosition() const override { return clockMs; }
    bool getGameProp(std::string_view, scene::Value&) override { return false; }
    bool setGameProp(std::string_view, const scene::Value&) override { return false; }
    void triggerEvent(const std::string&, const std::string&, const std::string&) override {}
    void debugPrint(const std::string&, uint32_t) override {}
    void debugNotice(const std::string&, bool, bool) override {}
    bool shadersEnabled() override { return shaders; }
};

// A line of news at the bottom of an editor.
struct Status {
    std::string text;
    double at = -10;
    bool bad = false;
    void set(const std::string& t, bool error = false) {
        text = t;
        bad = error;
        at = app().now;
    }
    void draw() const {
        if (app().now - at > 6 || text.empty()) return;
        const float al = float(std::min(1.0, 6 - (app().now - at)));
        const float w = gfx::measure(FontId::Ui, text, 18).x;
        const float y = app().touchMode ? (gfx::H - 96.0f) : (gfx::H - 70.0f);
        DrawRectangleRec({12, y, w + 20, 28}, fade(BLACK, 0.78f * al));
        gfx::text(FontId::Ui, text, 22, y + 4, 18, fade(bad ? Color{255, 120, 120, 255} : kGold, al));
    }
};

// ======================================================= chart converter ==
// Converts a mod's charts to Astralis's binary format (.ghc), each one next to
// its JSON (kept: Psych and other engines read that). When both are there the
// game loads the .ghc: a fraction of the size, read faster.

class ChartConverterState : public State {
public:
    void enter() override {
        mod_ = currentMod();
        rows();
    }
    ~ChartConverterState() override {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
    }
    void update(double) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (running_.load()) {
            if (in.back()) stop_ = true;  // stops after the chart being converted
            return;
        }
        if (worker_.joinable()) {  // finished: the song list sees the new files
            worker_.join();
            a.library.scan(a.base);
            rows();
        }
        if (in.back()) {
            sfxPlay(Sfx::Cancel);
            a.change(makeEditorMenu());
            return;
        }
        const int n = int(rows_.size());
        if (n == 0) return;
        if (in.up()) sel_ = (sel_ + n - 1) % n;
        if (in.down()) sel_ = (sel_ + 1) % n;
        if (in.accept()) start({size_t(sel_)});
        if (in.pressed(KEY_A)) {
            std::vector<size_t> all;
            for (size_t i = 0; i < rows_.size(); ++i) all.push_back(i);
            start(all);
        }
    }
    void draw() override {
        App& a = app();
        const bool touch = a.touchMode;
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x6C8CFF, menuPulse());
        const float boxBot = touch ? (gfx::H - 70.0f) : (gfx::H - 60.0f);
        DrawRectangleRec({40, 24, gfx::W - 80, boxBot - 24}, fade(BLACK, 0.65f));
        gfx::text(FontId::Ui, "Binary charts (.ghc)   mod: " + modLabel(mod_), 60, 38, 26, kGold);
        gfx::text(FontId::Ui, "Each chart gets a .ghc beside its JSON (the JSON stays, for Psych). The game loads the .ghc.", 60, 72,
                  17, fade(kIvory, 0.75f));
        gfx::text(FontId::Ui, "JSON", 700, 96, 15, fade(kIvory, 0.5f), Align::Right);
        gfx::text(FontId::Ui, ".ghc", 840, 96, 15, fade(kIvory, 0.5f), Align::Right);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (rows_.empty()) {
                gfx::text(FontId::Ui, "No charts in this mod.", 60, 130, 22, kIvory);
            } else if (!touch) {
                const int visible = 17, first = std::clamp(sel_ - visible / 2, 0, std::max(0, int(rows_.size()) - visible));
                for (int i = first; i < std::min(int(rows_.size()), first + visible); ++i) {
                    const Row& r = rows_[size_t(i)];
                    const float y = 116.0f + float(i - first) * 30.0f;
                    if (i == sel_) DrawRectangleRec({52, y - 3, gfx::W - 104, 28}, fade(kGold, 0.18f));
                    gfx::text(FontId::Ui, r.song + "  " + r.diff, 64, y, 19, i == sel_ ? kGold : kIvory);
                    gfx::text(FontId::Ui, r.json.empty() ? "-" : megabytes(r.jsonBytes), 700, y, 19, fade(kIvory, 0.8f), Align::Right);
                    gfx::text(FontId::Ui, r.ghc.empty() ? "-" : megabytes(r.ghcBytes), 840, y, 19, fade(kIvory, 0.8f), Align::Right);
                    gfx::text(FontId::Ui, r.note, 870, y, 17, r.bad ? Color{255, 130, 130, 255} : Color{150, 230, 170, 255});
                }
            } else {
                const Rectangle view{48, 114, gfx::W - 96, boxBot - 122};
                const float rowH = 40.0f;
                ui_.beginScroll("conv_list", view, scroll_);
                for (int i = 0; i < int(rows_.size()); ++i) {
                    const Row& r = rows_[size_t(i)];
                    const float y = view.y + float(i) * rowH - scroll_;
                    if (y + rowH < view.y || y > view.y + view.height) continue;
                    const Rectangle rr{view.x + 4, y + 2, view.width - 14, rowH - 4};
                    const bool hov = ui_.hover(rr);
                    if (i == sel_ || hov) DrawRectangleRounded(rr, 0.18f, 4, i == sel_ ? fade(kGold, 0.22f) : Color{56, 44, 86, 160});
                    gfx::text(FontId::Ui, r.song + "  " + r.diff, 64, y + 10, 19, i == sel_ ? kGold : kIvory);
                    gfx::text(FontId::Ui, r.json.empty() ? "-" : megabytes(r.jsonBytes), 700, y + 10, 19, fade(kIvory, 0.8f), Align::Right);
                    gfx::text(FontId::Ui, r.ghc.empty() ? "-" : megabytes(r.ghcBytes), 840, y + 10, 19, fade(kIvory, 0.8f), Align::Right);
                    gfx::text(FontId::Ui, r.note, 870, y + 11, 17, r.bad ? Color{255, 130, 130, 255} : Color{150, 230, 170, 255});
                    if (hov && ui_.tap() && !running_.load()) sel_ = i;
                }
                ui_.endScroll(view, scroll_, float(rows_.size()) * rowH + 6.0f);
            }
        }
        if (touch) {
            const float by = gfx::H - 60.0f;
            DrawRectangleRec({0, by, gfx::W, 60}, Color{10, 7, 18, 248});
            DrawRectangleRec({0, by, gfx::W, 1.5f}, Color{110, 92, 150, 170});
            const float y = by + 8.0f, h = 44.0f;
            if (ui_.button({20, y, 140, h}, running_.load() ? "STOP" : "< BACK")) {
                if (running_.load()) {
                    stop_ = true;
                } else {
                    sfxPlay(Sfx::Cancel);
                    a.change(makeEditorMenu());
                    gfx::endCanvas();
                    return;
                }
            }
            if (!running_.load() && !rows_.empty()) {
                if (ui_.button({gfx::W - 460, y, 210, h}, "CONVERT SELECTED")) start({size_t(sel_)});
                if (ui_.button({gfx::W - 236, y, 216, h}, "CONVERT ALL")) {
                    std::vector<size_t> all;
                    for (size_t i = 0; i < rows_.size(); ++i) all.push_back(i);
                    start(all);
                }
            } else if (running_.load()) {
                gfx::text(FontId::Ui, "Converting charts...", gfx::W * 0.5f, y + 11, 22, kGold, Align::Center);
            }
        }
        hints(running_.load() ? "Converting...   ESC stop" : "UP/DOWN choose   ENTER convert this one   A convert all   ESC back");
        gfx::endCanvas();
    }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        return t;
    }

private:
    struct Row {
        std::string song, diff, json, ghc, note;
        uint64_t jsonBytes = 0, ghcBytes = 0;
        bool bad = false;
    };

    static std::string megabytes(uint64_t b) {
        return b >= 1000000 ? TextFormat("%.1f MB", double(b) / 1e6) : TextFormat("%.1f KB", double(b) / 1e3);
    }

    // The mod's charts that are JSON or .ghc files (not FL Studio projects or
    // the V-Slice ones, which hold several difficulties).
    void rows() {
        std::lock_guard<std::mutex> lk(mtx_);
        rows_.clear();
        std::set<std::string> seen;
        for (const SongEntry& e : app().library.songs()) {
            if (e.builtin || !e.metaPath.empty()) continue;
            if (!mod_.name.empty() && e.pack != mod_.name) continue;
            for (size_t i = 0; i < e.diffs.size() && i < e.chartPaths.size(); ++i) {
                const std::string& p = e.chartPaths[i];
                std::string base;
                if (endsWithI(p, ".json")) base = p.substr(0, p.size() - 5);
                else if (endsWithI(p, kBinaryChartExt)) base = p.substr(0, p.size() - std::string(kBinaryChartExt).size());
                else continue;
                if (!seen.insert(base).second) continue;
                Row r;
                r.song = e.title;
                r.diff = e.diffs[i];
                std::error_code ec;
                if (fs::exists(toPath(base + ".json"), ec)) {
                    r.json = base + ".json";
                    r.jsonBytes = fileSize(r.json);
                }
                if (fs::exists(toPath(base + kBinaryChartExt), ec)) {
                    r.ghc = base + kBinaryChartExt;
                    r.ghcBytes = fileSize(r.ghc);
                }
                if (r.json.empty()) r.note = "only the .ghc";
                rows_.push_back(std::move(r));
            }
        }
        sel_ = std::clamp(sel_, 0, std::max(0, int(rows_.size()) - 1));
    }

    void start(std::vector<size_t> which) {
        if (running_.load() || which.empty()) return;
        stop_ = false;
        running_ = true;
        worker_ = std::thread([this, which] {
            for (size_t i : which) {
                if (stop_.load()) break;
                std::string json;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (i >= rows_.size() || rows_[i].json.empty()) continue;
                    json = rows_[i].json;
                    rows_[i].note = "converting...";
                    rows_[i].bad = false;
                }
                const std::string ghc = json.substr(0, json.size() - 5) + kBinaryChartExt;
                ConvertStats st;
                std::string err;
                const bool ok = convertChartToBinary(json, ghc, err, &st);
                std::lock_guard<std::mutex> lk(mtx_);
                Row& r = rows_[i];
                r.bad = !ok;
                if (ok) {
                    r.ghc = ghc;
                    r.ghcBytes = st.outBytes;
                    r.note = TextFormat("%.1fx smaller, %.2f s", double(st.inBytes) / double(std::max<uint64_t>(1, st.outBytes)), st.seconds);
                } else {
                    r.note = err;
                }
            }
            running_ = false;
        });
    }

    ModChoice mod_;
    Ui ui_;
    float scroll_ = 0.0f;
    std::vector<Row> rows_;
    int sel_ = 0;
    std::mutex mtx_;
    std::thread worker_;
    std::atomic<bool> running_{false}, stop_{false};
};

// ========================================================= script editor ==
// Issue #22: the mod's scripts (Lua, HScript) and shaders, edited in the
// game: a file list, line numbers, colours for keywords, strings and
// comments, undo, and saving with a .bak of the file as it was.

class ScriptEditorState : public State {
public:
    void enter() override {
        mod_ = currentMod();
        filesOpen_ = !app().touchMode;
        listFiles();
        if (!files_.empty()) open(0);
    }
    bool keyReserved(int) const override { return true; }  // every key types

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (editingLine_) {
            std::string res;
            const platform::TextAsk st = platform::textStatus(&res);
            if (st == platform::TextAsk::Done) {
                if (editLineIdx_ >= 0 && editLineIdx_ < int(lines_.size()) && res != lines_[size_t(editLineIdx_)]) {
                    remember();
                    lines_[size_t(editLineIdx_)] = res;
                    cx_ = int(res.size());
                }
                platform::resetText();
                editingLine_ = false;
            } else if (st == platform::TextAsk::Cancelled) {
                platform::resetText();
                editingLine_ = false;
            }
            return;
        }
        const bool ctrl = in.isDown(KEY_LEFT_CONTROL) || in.isDown(KEY_RIGHT_CONTROL);
        if (in.pressed(KEY_ESCAPE)) {
            tryLeave();
            return;
        }
        if (ctrl && in.pressed(KEY_S)) save();
        if (ctrl && in.pressed(KEY_Z)) undo();
        if (ctrl && in.pressed(KEY_N)) newFile();
        if (ctrl && (in.pressed(KEY_PAGE_UP) || in.pressed(KEY_PAGE_DOWN)) && !files_.empty()) {
            const int n = int(files_.size());
            open(size_t(((cur_ + (in.pressed(KEY_PAGE_UP) ? -1 : 1)) % n + n) % n));
            return;
        }
        if (!a.touchMode) mouse(in);
        if (lines_.empty()) lines_.push_back("");
        if (!ctrl)
            for (char32_t c : in.typed())
                if (c >= 32 && c != 127) insert(utf8(c));
        auto hit = [&](int key) { return repeat(in, key, dt); };
        if (hit(KEY_ENTER) || hit(KEY_KP_ENTER)) newline();
        if (hit(KEY_TAB)) insert("    ");
        if (hit(KEY_BACKSPACE)) backspace();
        if (hit(KEY_DELETE)) del();
        if (hit(KEY_LEFT)) left();
        if (hit(KEY_RIGHT)) right();
        if (hit(KEY_UP)) moveLine(-1);
        if (hit(KEY_DOWN)) moveLine(1);
        if (hit(KEY_PAGE_UP) && !ctrl) moveLine(-kRows);
        if (hit(KEY_PAGE_DOWN) && !ctrl) moveLine(kRows);
        if (in.pressed(KEY_HOME)) cx_ = 0;
        if (in.pressed(KEY_END)) cx_ = int(lines_[size_t(cy_)].size());
        if (!a.touchMode) {
            if (cy_ < top_) top_ = cy_;
            if (cy_ >= top_ + kRows) top_ = cy_ - kRows + 1;
        }
    }

    void draw() override {
        const bool touch = app().touchMode;
        gfx::beginCanvas();
        ClearBackground(Color{14, 10, 22, 255});
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{14, 10, 22, 255});
        if (!touch) {
            // Files
            DrawRectangleRec({0, 0, kListW, gfx::H}, Color{10, 7, 16, 255});
            gfx::text(FontId::Ui, "Scripts: " + modLabel(mod_), 12, 10, 18, kGold);
            for (size_t i = 0; i < files_.size() && i < 40; ++i) {
                const float y = 40.0f + float(i) * 17.0f;
                if (int(i) == cur_) DrawRectangleRec({4, y - 1, kListW - 8, 17}, fade(kGold, 0.2f));
                gfx::text(FontId::Ui, files_[i].label, 10, y, 14, int(i) == cur_ ? kGold : fade(kIvory, 0.75f));
            }
            // Text
            const float x0 = kListW + 56, y0 = 40;
            gfx::text(FontId::Ui, (cur_ >= 0 ? files_[size_t(cur_)].label : std::string("(no file)")) + (dirty_ ? "  *" : ""), kListW + 12,
                      10, 18, kIvory);
            for (int k = 0; k < kRows && top_ + k < int(lines_.size()); ++k) {
                const int ln = top_ + k;
                const float y = y0 + float(k) * kLineH;
                if (ln == cy_) DrawRectangleRec({kListW, y - 1, gfx::W - kListW, kLineH}, Color{30, 22, 46, 255});
                gfx::text(FontId::Ui, std::to_string(ln + 1), x0 - 10, y, 15, fade(kIvory, 0.35f), Align::Right);
                drawLine(lines_[size_t(ln)], x0, y);
            }
            if (cy_ >= top_ && cy_ < top_ + kRows && std::fmod(GetTime(), 1.0) < 0.6) {
                const std::string& l = lines_[size_t(cy_)];
                const float cx = x0 + gfx::measure(FontId::Ui, l.substr(0, size_t(std::min<int>(cx_, int(l.size())))), 16).x;
                DrawRectangleRec({cx, y0 + float(cy_ - top_) * kLineH, 2, kLineH - 2}, kGold);
            }
            if (!status_.empty()) gfx::text(FontId::Ui, status_, kListW + 12, gfx::H - 52, 16, kGold);
            hints("Type to edit   CTRL+S save   CTRL+Z undo   CTRL+N new script   CTRL+PGUP/PGDN file   click a file   ESC back");
            gfx::endCanvas();
            return;
        }
        // Touch script editor: top bar, collapsible scrollable files drawer, tap-to-edit code lines, bottom toolbar.
        const float topH = 48.0f, botY = gfx::H - 60.0f;
        DrawRectangleRec({0, 0, gfx::W, topH}, Color{8, 5, 14, 245});
        DrawRectangleRec({0, topH - 1, gfx::W, 1}, Color{110, 92, 150, 150});
        if (ui_.button({8, 6, 94, 36}, "< BACK")) {
            if (tryLeave()) {
                gfx::endCanvas();
                return;
            }
        }
        if (ui_.button({108, 6, 98, 36}, filesOpen_ ? "FILES <" : "FILES >", filesOpen_)) filesOpen_ = !filesOpen_;
        const std::string title = (cur_ >= 0 ? files_[size_t(cur_)].label : std::string("(no file)")) + (dirty_ ? "  *" : "");
        gfx::text(FontId::Ui, title, 220, 14, 20, kIvory);
        if (ui_.button({gfx::W - 118, 6, 110, 36}, dirty_ ? "SAVE *" : "SAVE", dirty_)) save();

        const float codeLeft = filesOpen_ ? 316.0f : 6.0f;
        if (filesOpen_) {
            const Rectangle fBox{4, topH + 4, 306, botY - topH - 8};
            DrawRectangleRounded(fBox, 0.02f, 6, Color{10, 7, 16, 245});
            DrawRectangleRoundedLinesEx(fBox, 0.02f, 6, 1.5f, Color{110, 92, 150, 140});
            const Rectangle fView{fBox.x + 4, fBox.y + 4, fBox.width - 8, fBox.height - 8};
            const float fRow = 36.0f;
            ui_.beginScroll("script_files", fView, filesScroll_);
            for (size_t i = 0; i < files_.size(); ++i) {
                const float y = fView.y + float(i) * fRow - filesScroll_;
                if (y + fRow < fView.y || y > fView.y + fView.height) continue;
                const Rectangle r{fView.x + 2, y + 2, fView.width - 10, fRow - 4};
                const bool hov = ui_.hover(r);
                if (int(i) == cur_ || hov) DrawRectangleRounded(r, 0.18f, 4, int(i) == cur_ ? fade(kGold, 0.24f) : Color{54, 42, 82, 180});
                gfx::text(FontId::Ui, files_[i].label, r.x + 8, r.y + 7, 16, int(i) == cur_ ? kGold : kIvory);
                if (hov && ui_.tap()) {
                    open(i);
                    filesOpen_ = false;
                }
            }
            ui_.endScroll(fView, filesScroll_, float(files_.size()) * fRow + 8.0f);
        }

        const Rectangle cView{codeLeft, topH + 4, gfx::W - codeLeft - 6, botY - topH - (status_.empty() ? 8.0f : 32.0f)};
        const float lineH = 28.0f;
        ui_.beginScroll("script_code", cView, codeScroll_);
        const float x0 = cView.x + 56.0f;
        for (int ln = 0; ln < int(lines_.size()); ++ln) {
            const float y = cView.y + float(ln) * lineH - codeScroll_;
            if (y + lineH < cView.y || y > cView.y + cView.height) continue;
            const Rectangle lr{cView.x, y, cView.width - 8, lineH};
            const bool hov = ui_.hover(lr);
            if (ln == cy_) DrawRectangleRec(lr, Color{42, 30, 68, 255});
            else if (hov) DrawRectangleRec(lr, Color{26, 20, 42, 200});
            gfx::text(FontId::Ui, std::to_string(ln + 1), x0 - 10, y + 5, 16, ln == cy_ ? kGold : fade(kIvory, 0.4f), Align::Right);
            drawLine(lines_[size_t(ln)], x0, y + 5);
            if (hov && ui_.tap()) {
                if (cy_ == ln) editSelectedLine();
                else {
                    cy_ = ln;
                    cx_ = int(lines_[size_t(ln)].size());
                }
            }
        }
        ui_.endScroll(cView, codeScroll_, float(lines_.size()) * lineH + 24.0f);
        if (!status_.empty()) gfx::text(FontId::Ui, status_, codeLeft + 10, botY - 24, 16, kGold);

        // Bottom docked mobile editing bar
        DrawRectangleRec({0, botY, gfx::W, 60}, Color{10, 7, 18, 248});
        DrawRectangleRec({0, botY, gfx::W, 1.5f}, Color{110, 92, 150, 170});
        const float by = botY + 8.0f, bh = 44.0f;
        if (ui_.button({12, by, 148, bh}, "EDIT LINE")) editSelectedLine();
        if (ui_.button({168, by, 126, bh}, "+ LINE")) {
            remember();
            if (lines_.empty()) lines_.push_back("");
            cy_ = std::clamp(cy_, 0, int(lines_.size()) - 1);
            lines_.insert(lines_.begin() + cy_ + 1, "");
            ++cy_;
            cx_ = 0;
            editSelectedLine();
        }
        if (ui_.button({302, by, 128, bh}, "DEL LINE")) {
            if (!lines_.empty()) {
                remember();
                if (lines_.size() == 1) lines_[0].clear();
                else {
                    lines_.erase(lines_.begin() + cy_);
                    cy_ = std::clamp(cy_, 0, int(lines_.size()) - 1);
                }
                cx_ = 0;
            }
        }
        if (ui_.button({442, by, 124, bh}, "INDENT")) {
            if (cy_ >= 0 && cy_ < int(lines_.size())) {
                remember();
                lines_[size_t(cy_)].insert(0, "    ");
            }
        }
        if (ui_.button({574, by, 136, bh}, "UNINDENT")) {
            if (cy_ >= 0 && cy_ < int(lines_.size())) {
                std::string& l = lines_[size_t(cy_)];
                int rm = 0;
                while (rm < 4 && rm < int(l.size()) && l[size_t(rm)] == ' ') ++rm;
                if (rm == 0 && !l.empty() && l[0] == '\t') rm = 1;
                if (rm > 0) {
                    remember();
                    l.erase(0, size_t(rm));
                }
            }
        }
        if (ui_.button({726, by, 114, bh}, "UNDO")) undo();
        if (ui_.button({848, by, 154, bh}, "NEW SCRIPT")) newFile();
        if (ui_.button({1010, by, 126, bh}, "SAVE")) save();
        gfx::endCanvas();
    }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        return t;
    }

private:
    static constexpr float kListW = 300, kLineH = 19;
    static constexpr int kRows = 33;

    bool tryLeave() {
        if (dirty_ && !leaveArmed_) {
            status_ = app().touchMode ? "Unsaved changes: tap BACK again to leave, or tap SAVE"
                                      : "Unsaved changes: ESC again to leave without saving (CTRL+S saves)";
            leaveArmed_ = true;
            return false;
        }
        app().change(makeEditorMenu());
        return true;
    }

    void editSelectedLine() {
        if (lines_.empty()) lines_.push_back("");
        cy_ = std::clamp(cy_, 0, int(lines_.size()) - 1);
        if (platform::askText("Line " + std::to_string(cy_ + 1), lines_[size_t(cy_)])) {
            editingLine_ = true;
            editLineIdx_ = cy_;
        } else {
            status_ = "Line " + std::to_string(cy_ + 1) + " selected (type on keyboard)";
        }
    }
    struct File {
        std::string path, label;
    };

    void listFiles() {
        files_.clear();
        static const char* exts[] = {".lua", ".hx", ".hxs", ".hscript", ".frag", ".vert"};
        for (const std::string& root : mod_.roots) {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(toPath(root), ec), end; !ec && it != end && files_.size() < 2000; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string p = pathStr(it->path());
                for (const char* e : exts)
                    if (endsWithI(p, e)) {
                        std::string label = pathStr(fs::relative(it->path(), toPath(root), ec));
                        files_.push_back({p, label});
                        break;
                    }
            }
        }
        std::sort(files_.begin(), files_.end(), [](const File& a, const File& b) { return a.label < b.label; });
    }

    void open(size_t i) {
        if (i >= files_.size()) return;
        if (dirty_) save();
        cur_ = int(i);
        lines_.clear();
        std::string text;
        if (std::FILE* f = std::fopen(files_[i].path.c_str(), "rb")) {
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        std::string line;
        for (char c : text) {
            if (c == '\r') continue;
            if (c == '\n') {
                lines_.push_back(line);
                line.clear();
            } else {
                line += c;
            }
        }
        lines_.push_back(line);
        cy_ = cx_ = top_ = 0;
        undo_.clear();
        dirty_ = leaveArmed_ = false;
        backedUp_ = false;
        status_.clear();
    }

    void save() {
        if (cur_ < 0) return;
        const std::string& p = files_[size_t(cur_)].path;
        std::error_code ec;
        if (!backedUp_ && fs::exists(toPath(p), ec) && !fs::exists(toPath(p + ".bak"), ec)) fs::copy_file(toPath(p), toPath(p + ".bak"), ec);
        backedUp_ = true;
        std::string text;
        for (size_t i = 0; i < lines_.size(); ++i) text += lines_[i] + (i + 1 < lines_.size() ? "\n" : "");
        std::FILE* f = std::fopen(p.c_str(), "wb");
        const bool ok = f && std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (f) std::fclose(f);
        status_ = ok ? "Saved " + files_[size_t(cur_)].label : "Could not save " + p;
        if (ok) dirty_ = false;
    }

    void newFile() {
        if (mod_.roots.empty()) return;
        if (dirty_) save();
        const fs::path dir = toPath(mod_.roots[0]) / "scripts";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path p;
        for (int k = 1; k < 1000; ++k) {
            p = dir / ("new_script" + (k > 1 ? std::to_string(k) : std::string()) + ".lua");
            if (!fs::exists(p, ec)) break;
        }
        std::FILE* f = std::fopen(pathStr(p).c_str(), "wb");
        if (!f) {
            status_ = "Could not make " + pathStr(p);
            return;
        }
        const char* tmpl = "function onCreate()\n    -- runs when the song starts\nend\n\nfunction onUpdate(elapsed)\nend\n";
        std::fwrite(tmpl, 1, std::strlen(tmpl), f);
        std::fclose(f);
        listFiles();
        for (size_t i = 0; i < files_.size(); ++i)
            if (files_[i].path == pathStr(p)) open(i);
        status_ = "New script: " + pathStr(p.filename()) + " (scripts/ runs in every song of the mod)";
    }

    // A key held repeats (as in any text editor).
    bool repeat(const Input& in, int key, double dt) {
        if (in.pressed(key)) {
            held_[key] = 0;
            return true;
        }
        if (!in.isDown(key)) {
            held_.erase(key);
            return false;
        }
        double& t = held_[key];
        const double before = t;
        t += dt;
        return t > 0.4 && std::floor((t - 0.4) / 0.035) != std::floor((before - 0.4) / 0.035);
    }

    void mouse(const Input& in) {
        const Vector2 m = gfx::toCanvas(GetMousePosition());
        if (in.wheel() != 0 && m.x > kListW) {
            top_ = std::clamp(top_ - int(in.wheel() * 3), 0, std::max(0, int(lines_.size()) - 1));
        }
        if (!in.mousePressed(MOUSE_BUTTON_LEFT)) return;
        if (m.x < kListW) {
            const int i = int((m.y - 40) / 17);
            if (i >= 0 && i < int(files_.size())) open(size_t(i));
            return;
        }
        const int ln = top_ + int((m.y - 40) / kLineH);
        if (ln < 0 || ln >= int(lines_.size())) return;
        cy_ = ln;
        const std::string& l = lines_[size_t(ln)];
        const float x0 = kListW + 56;
        cx_ = int(l.size());
        for (size_t k = 0; k <= l.size(); ++k)
            if (x0 + gfx::measure(FontId::Ui, l.substr(0, k), 16).x > m.x) {
                cx_ = int(k == 0 ? 0 : k - 1);
                break;
            }
    }

    void remember() {
        undo_.push_back({lines_, {cy_, cx_}});
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        dirty_ = true;
        leaveArmed_ = false;
    }
    void undo() {
        if (undo_.empty()) return;
        lines_ = std::move(undo_.back().first);
        cy_ = undo_.back().second.first;
        cx_ = undo_.back().second.second;
        undo_.pop_back();
        dirty_ = true;
    }
    std::string& line() { return lines_[size_t(cy_)]; }
    void clampCx() { cx_ = std::clamp(cx_, 0, int(line().size())); }
    void insert(const std::string& s) {
        clampCx();
        // One undo step per word typed, not per letter.
        if (undo_.empty() || s == " " || s.size() > 1) remember();
        line().insert(size_t(cx_), s);
        cx_ += int(s.size());
        dirty_ = true;
    }
    void newline() {
        remember();
        clampCx();
        std::string rest = line().substr(size_t(cx_));
        line().erase(size_t(cx_));
        size_t indent = 0;  // the new line starts where this one does
        while (indent < line().size() && (line()[indent] == ' ' || line()[indent] == '\t')) ++indent;
        const std::string pad = line().substr(0, indent);
        lines_.insert(lines_.begin() + cy_ + 1, pad + rest);
        ++cy_;
        cx_ = int(pad.size());
    }
    void backspace() {
        clampCx();
        if (cx_ > 0) {
            remember();
            int k = cx_ - 1;
            while (k > 0 && (static_cast<unsigned char>(line()[size_t(k)]) & 0xC0) == 0x80) --k;  // a whole UTF-8 letter
            line().erase(size_t(k), size_t(cx_ - k));
            cx_ = k;
        } else if (cy_ > 0) {
            remember();
            cx_ = int(lines_[size_t(cy_ - 1)].size());
            lines_[size_t(cy_ - 1)] += line();
            lines_.erase(lines_.begin() + cy_);
            --cy_;
        }
    }
    void del() {
        clampCx();
        if (cx_ < int(line().size())) {
            remember();
            int k = cx_ + 1;
            while (k < int(line().size()) && (static_cast<unsigned char>(line()[size_t(k)]) & 0xC0) == 0x80) ++k;
            line().erase(size_t(cx_), size_t(k - cx_));
        } else if (cy_ + 1 < int(lines_.size())) {
            remember();
            line() += lines_[size_t(cy_ + 1)];
            lines_.erase(lines_.begin() + cy_ + 1);
        }
    }
    void left() {
        clampCx();
        if (cx_ > 0) {
            --cx_;
            while (cx_ > 0 && (static_cast<unsigned char>(line()[size_t(cx_)]) & 0xC0) == 0x80) --cx_;
        } else if (cy_ > 0) {
            --cy_;
            cx_ = int(line().size());
        }
    }
    void right() {
        clampCx();
        if (cx_ < int(line().size())) {
            ++cx_;
            while (cx_ < int(line().size()) && (static_cast<unsigned char>(line()[size_t(cx_)]) & 0xC0) == 0x80) ++cx_;
        } else if (cy_ + 1 < int(lines_.size())) {
            ++cy_;
            cx_ = 0;
        }
    }
    void moveLine(int d) { cy_ = std::clamp(cy_ + d, 0, int(lines_.size()) - 1); }

    // Keywords, strings, numbers and comments in their colours.
    void drawLine(const std::string& l, float x, float y) const {
        static const std::set<std::string> kw = {
            "function", "end", "if", "then", "else", "elseif", "local", "return", "for", "in", "do", "while", "repeat",
            "until", "and", "or", "not", "nil", "true", "false", "var", "final", "new", "import", "class", "public",
            "private", "static", "override", "null", "switch", "case", "default", "break", "continue", "try", "catch",
            "uniform", "void", "float", "vec2", "vec3", "vec4", "int", "bool", "sampler2D", "this", "extends"};
        const bool luaFile = cur_ >= 0 && endsWithI(files_[size_t(cur_)].path, ".lua");
        size_t i = 0;
        float cx = x;
        auto put = [&](const std::string& s, Color c) {
            gfx::text(FontId::Ui, s, cx, y, 16, c);
            cx += gfx::measure(FontId::Ui, s, 16).x;
        };
        while (i < l.size() && cx < gfx::W) {
            const char c = l[i];
            const bool comment = luaFile ? l.compare(i, 2, "--") == 0 : l.compare(i, 2, "//") == 0;
            if (comment) {
                put(l.substr(i), Color{120, 130, 150, 255});
                break;
            }
            if (c == '"' || c == '\'') {
                size_t j = i + 1;
                while (j < l.size() && l[j] != c) j += l[j] == '\\' ? 2 : 1;
                j = std::min(j + 1, l.size());
                put(l.substr(i, j - i), Color{150, 220, 140, 255});
                i = j;
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i;
                while (j < l.size() && (std::isalnum(static_cast<unsigned char>(l[j])) || l[j] == '_')) ++j;
                const std::string w = l.substr(i, j - i);
                put(w, kw.count(w) ? Color{255, 170, 110, 255} : kInk);
                i = j;
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                size_t j = i;
                while (j < l.size() && (std::isalnum(static_cast<unsigned char>(l[j])) || l[j] == '.')) ++j;
                put(l.substr(i, j - i), Color{130, 190, 255, 255});
                i = j;
            } else {
                size_t j = i + 1;
                while (j < l.size() && !std::isalnum(static_cast<unsigned char>(l[j])) && l[j] != '_' && l[j] != '"' && l[j] != '\'' &&
                       l.compare(j, 2, luaFile ? "--" : "//") != 0)
                    ++j;
                put(l.substr(i, j - i), fade(kInk, 0.85f));
                i = j;
            }
        }
    }

    ModChoice mod_;
    Ui ui_;
    bool filesOpen_ = true;
    float filesScroll_ = 0.0f, codeScroll_ = 0.0f;
    bool editingLine_ = false;
    int editLineIdx_ = -1;
    std::vector<File> files_;
    int cur_ = -1;
    std::vector<std::string> lines_;
    int cy_ = 0, cx_ = 0, top_ = 0;
    std::vector<std::pair<std::vector<std::string>, std::pair<int, int>>> undo_;
    std::map<int, double> held_;
    bool dirty_ = false, leaveArmed_ = false, backedUp_ = false;
    std::string status_;
};

// ============================================================ editor menu ==

class EditorMenuState : public State {
public:
    void update(double) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        const int mods = int(modChoices().size());
        if (in.up()) { sel_ = (sel_ + kItems - 1) % kItems; sfxPlay(Sfx::Scroll); }
        if (in.down()) { sel_ = (sel_ + 1) % kItems; sfxPlay(Sfx::Scroll); }
        if ((in.left() || in.right()) && mods > 1) {
            int& m = modIndex();
            m = ((m + (in.left() ? -1 : 1)) % mods + mods) % mods;
            sfxPlay(Sfx::Scroll);
        }
        if (in.back()) {
            sfxPlay(Sfx::Cancel);
            a.change(makeMainMenu());
            return;
        }
        if (!in.accept()) return;
        launch(sel_);
    }
    void draw() override {
        App& a = app();
        const bool touch = a.touchMode;
        const bool psychStyle = a.psychMenusActive();
        const PsychUi& ui = psychUi();
        static const char* items[kItems] = {"Chart Editor",    "Modchart Editor", "SNIFF",         "Character Editor",
                                            "Stage Editor",    "Event Editor",    "Script Editor", "Chart Converter"};
        static const char* about[kItems] = {"Edit a song's notes, sections, events, and JSON/GHC save options",
                                            "Build animated strum, camera, and wave modcharts (saves modchart.lua)",
                                            "Open FL Studio (.flp/.fsc) charts with custom multikey MIDI ranges",
                                            "Animations, offsets, image, icon and colours of a character",
                                            "Zoom, where the characters stand and what the camera sees",
                                            "New events for charts: a description and a Lua script",
                                            "The mod's Lua and HScript scripts and its shaders",
                                            "Convert charts to binary .ghc files: much smaller, loaded faster"};
        gfx::beginCanvas();
        if (psychStyle) {
            drawPsychMenuBackground(1, Color{53, 53, 53, 255}, 0.0f, false, a.now, 0x353535, menuPulse());
            for (int i = 0; i < kItems; ++i) {
                const bool on = i == sel_;
                const float y = (touch ? 56.0f : 44.0f) + float(i) * (touch ? 63.0f : 65.0f);
                const float al = on ? 1.0f : 0.55f;
                const Rectangle hit{gfx::W * 0.5f - 290.0f, y - 5.0f, 580.0f, 56.0f};
                if (touch) DrawRectangleRounded(hit, 0.2f, 6, on ? Color{90, 66, 140, 140} : Color{20, 14, 34, 120});
                if (ui.alphabetCentered(items[i], gfx::W * 0.5f, y, on ? 48.0f : 38.0f, al) < 0)
                    ui.uiText(items[i], gfx::W * 0.5f, y + 4, on ? 40.0f : 32.0f, fade(WHITE, al), 0, 2.0f);
                if (touch && ui_.hover(hit) && ui_.tap()) {
                    sel_ = i;
                    launch(i);
                    gfx::endCanvas();
                    return;
                }
            }
            const ModChoice m = currentMod();
            DrawRectangleRec({0, gfx::H - 134, gfx::W, 134}, Color{0, 0, 0, 185});
            ui.uiText(about[sel_], gfx::W * 0.5f, gfx::H - 124, 18, WHITE, 0, 1.0f);
            ui.uiText("<  Current Mod Directory: " + modLabel(m) + "  >", gfx::W * 0.5f, gfx::H - 92, 22, kGold, 0, 1.25f);
            ui.uiText("Saves into: " + m.roots[0], gfx::W * 0.5f, gfx::H - 60, 15, fade(WHITE, 0.72f), 0, 1.0f);
            if (!touch) {
                DrawRectangleRec({0, gfx::H - 40, gfx::W, 40}, Color{0, 0, 0, 195});
                ui.uiText("UP/DOWN choose   LEFT/RIGHT mod   ENTER open   ESC back", gfx::W / 2, gfx::H - 31, 18, WHITE, 0, 1.0f);
            }
        } else {
            drawMenuBackground(a.now, 0x6C8CFF, menuPulse());
            gfx::text(FontId::Title, "EDITORS", gfx::W / 2, touch ? 18.0f : 36.0f, touch ? 44.0f : 56.0f, kGold, Align::Center, 2.0f);
            for (int i = 0; i < kItems; ++i) {
                const bool on = i == sel_;
                const float y = (touch ? 76.0f : 104.0f) + i * 56.0f;
                const Rectangle hit{gfx::W * 0.5f - 290.0f, y - 4.0f, 580.0f, 50.0f};
                if (touch) {
                    DrawRectangleRounded(hit, 0.22f, 6, on ? Color{90, 66, 140, 170} : Color{24, 18, 42, 165});
                    DrawRectangleRoundedLinesEx(hit, 0.22f, 6, 1.5f, on ? kGold : Color{110, 92, 150, 160});
                }
                gfx::text(FontId::Title, items[i], gfx::W / 2, y + (touch ? 4.0f : 0.0f), on ? 40.0f : 32.0f, on ? kGold : fade(kIvory, 0.75f),
                          Align::Center, 2.0f);
                if (on && !touch) gfx::text(FontId::Ui, about[i], gfx::W / 2, y + 40, 16, fade(kIvory, 0.85f), Align::Center, 1.5f);
                if (touch && ui_.hover(hit) && ui_.tap()) {
                    sel_ = i;
                    launch(i);
                    gfx::endCanvas();
                    return;
                }
            }
            const ModChoice m = currentMod();
            gfx::text(FontId::Ui, "<  Mod: " + modLabel(m) + "  >", gfx::W / 2, gfx::H - 108, 24, kIvory, Align::Center, 1.5f);
            gfx::text(FontId::Ui, "The editors save into " + m.roots[0], gfx::W / 2, gfx::H - 72, 16, fade(kIvory, 0.6f), Align::Center, 1.0f);
            hints("UP/DOWN choose   LEFT/RIGHT mod   ENTER open   ESC back");
        }
        if (touch) {
            if (ui_.button({18, 16, 118, 44}, "< BACK")) {
                sfxPlay(Sfx::Cancel);
                a.change(makeMainMenu());
                gfx::endCanvas();
                return;
            }
            const int mods = int(modChoices().size());
            if (mods > 1) {
                if (ui_.button({120, gfx::H - 108, 96, 44}, "< MOD")) {
                    int& m = modIndex();
                    m = ((m - 1) % mods + mods) % mods;
                    sfxPlay(Sfx::Scroll);
                }
                if (ui_.button({gfx::W - 216, gfx::H - 108, 96, 44}, "MOD >")) {
                    int& m = modIndex();
                    m = (m + 1) % mods;
                    sfxPlay(Sfx::Scroll);
                }
            }
        }
        gfx::endCanvas();
    }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        return t;
    }

private:
    static constexpr int kItems = 8;
    int sel_ = 0;
    Ui ui_;

    void launch(int idx) {
        App& a = app();
        sfxPlay(Sfx::Confirm);
        switch (idx) {
            case 0: openChartEditor(); break;
            case 1: a.change(makeModchartEditor()); break;
            case 2: a.change(makeSniffEditor()); break;
            case 3: a.change(makeCharacterEditor()); break;
            case 4: a.change(makeStageEditor()); break;
            case 5: a.change(makeEventEditor()); break;
            case 6: a.change(makeScriptEditor()); break;
            default: a.change(makeChartConverter()); break;
        }
    }

    void openChartEditor() {
        App& a = app();
        const ModChoice m = currentMod();
        const auto& songs = a.library.songs();
        const SongEntry* pick = nullptr;
        if (a.freeplaySel >= 0 && a.freeplaySel < int(songs.size())) {
            const SongEntry& e = songs[size_t(a.freeplaySel)];
            if (!e.builtin && (m.name.empty() || e.pack == m.name)) pick = &e;
        }
        for (size_t i = 0; !pick && i < songs.size(); ++i)
            if (!songs[i].builtin && !songs[i].diffs.empty() && (m.name.empty() || songs[i].pack == m.name)) pick = &songs[i];
        if (!pick || pick->diffs.empty()) {
            a.notify("No song in this mod to chart: choose one in Freeplay and press 7 there");
            a.change(makeFreeplay());
            return;
        }
        const int d = clampv(a.freeplayDiff, 0, int(pick->diffs.size()) - 1);
        a.change(makeChartEditor(pick->id, pick->diffs[size_t(d)]));
    }
};

// ======================================================= character editor ==

class CharacterEditorState : public State {
public:
    void enter() override {
        mod_ = currentMod();
        panelOpen_ = !app().touchMode;
        useMod(sc_, mod_);
        names_ = listNames(sc_, "characters/", ".json");
        auto it = std::find(names_.begin(), names_.end(), "bf");
        load(it != names_.end() ? *it : names_.empty() ? std::string("bf") : names_[0]);
    }
    bool keyReserved(int) const override { return !ui_.focus.empty(); }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        return t;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (ch_) ch_->updateAnim(dt);
        const bool overChrome = a.touchMode && (ui_.mouse.y < 52.0f || ui_.mouse.y > gfx::H - 62.0f ||
                                                (ui_.mouse.x < 192.0f && ui_.mouse.y > gfx::H - 224.0f));
        view_.handle(in, ui_, overPanel(ui_, !a.touchMode || panelOpen_, panelX()) || overChrome, a.touchMode);
        if (!ui_.focus.empty()) return;
        const bool shift = in.isDown(KEY_LEFT_SHIFT) || in.isDown(KEY_RIGHT_SHIFT);
        const bool ctrl = in.isDown(KEY_LEFT_CONTROL) || in.isDown(KEY_RIGHT_CONTROL);
        if (in.pressed(KEY_ESCAPE)) {
            tryLeave();
            return;
        }
        if (ctrl && in.pressed(KEY_S)) save();
        const int n = int(def_.anims.size());
        if (n > 0 && (in.pressed(KEY_W) || in.pressed(KEY_S)) && !ctrl) selectAnim((anim_ + (in.pressed(KEY_W) ? n - 1 : 1)) % n);
        if (in.pressed(KEY_SPACE)) playSelected();
        if (in.pressed(KEY_G)) ghostOn_ = !ghostOn_;
        if (in.pressed(KEY_R) && !ctrl) resetView();
        const int step = shift ? 10 : 1;
        int dx = 0, dy = 0;
        if (in.pressed(KEY_LEFT)) dx += step;  // Psych: the arrows move the picture, its offset goes the other way
        if (in.pressed(KEY_RIGHT)) dx -= step;
        if (in.pressed(KEY_UP)) dy += step;
        if (in.pressed(KEY_DOWN)) dy -= step;
        if ((dx || dy) && anim_ < n) {
            psych::AnimDef& ad = def_.anims[size_t(anim_)];
            ad.offX += float(dx);
            ad.offY += float(dy);
            offsetsChanged();
        }
    }

    void draw() override {
        gfx::beginCanvas();
        view_.apply(sc_.camGame);
        drawGrid(view_);
        rend_.drawCamera(sc_, scene::Cam::Game, nullptr, nullptr);
        if (ch_) {
            // Where the character stands (its position) and where the camera looks.
            cross(view_.toCanvas(ch_->x, ch_->y), 10, Color{120, 200, 255, 255});
            const float mx = ch_->x + ch_->width * 0.5f, my = ch_->y + ch_->height * 0.5f;
            const Vector2 camPt = player_ ? Vector2{mx - 100 - def_.camX, my - 100 + def_.camY} : Vector2{mx + 150 + def_.camX, my - 100 + def_.camY};
            cross(view_.toCanvas(camPt.x, camPt.y), 16, kGold);
        } else {
            gfx::text(FontId::Ui, error_.empty() ? "No character" : error_, 430, 340, 22, Color{255, 140, 140, 255}, Align::Center, 1.5f);
        }
        if (drawInfo()) {
            gfx::endCanvas();
            return;
        }
        if (app().touchMode) drawTouchControls();
        drawPanel();
        status_.draw();
        gfx::endCanvas();
    }

private:
    scene::Scene sc_;
    SceneRenderer rend_;
    Ui ui_;
    View view_;
    Status status_;
    ModChoice mod_;
    std::vector<std::string> names_;
    int nameIdx_ = 0;
    psych::CharacterDef def_;
    std::string loadedPath_, loadedText_, saveName_, image_, icon_, error_;
    scene::CharacterPtr ch_, ghost_;
    std::vector<std::string> prefixes_;
    int anim_ = 0, tab_ = 0, prefixIdx_ = 0;
    bool player_ = true, ghostOn_ = true, dirty_ = false, leaveArmed_ = false;
    std::string fName_, fPrefix_, fIndices_;
    double fFps_ = 24;
    bool fLoop_ = false;
    float listScroll_ = 0;
    bool panelOpen_ = true, nudgeBig_ = false;
    float panelScroll_ = 0.0f;

    float panelX() const { return app().touchMode ? 836.0f : kPanelX; }
    float panelW() const { return gfx::W - panelX() - 8.0f; }

    bool tryLeave() {
        if (dirty_ && !leaveArmed_) {
            status_.set(app().touchMode ? "Unsaved changes: tap BACK again to leave, or tap SAVE"
                                        : "Unsaved changes: ESC again to leave without saving (CTRL+S saves)");
            leaveArmed_ = true;
            return false;
        }
        app().change(makeEditorMenu());
        return true;
    }

    void load(const std::string& name) {
        std::string err;
        if (!loadCharacterDef(sc_, name, def_, loadedPath_, loadedText_, err)) {
            status_.set(err, true);
            if (def_.images.empty()) def_.images = {"characters/" + name};
        }
        saveName_ = name;
        auto it = std::find(names_.begin(), names_.end(), name);
        nameIdx_ = it == names_.end() ? 0 : int(it - names_.begin());
        player_ = name.rfind("bf", 0) == 0 || name.find("player") != std::string::npos || name.find("boyfriend") != std::string::npos;
        anim_ = 0;
        dirty_ = leaveArmed_ = false;
        syncFields();
        rebuild();
        resetView();
    }

    void syncFields() {
        image_ = imageKeys(def_);
        icon_ = def_.icon;
        fillAnimFields();
    }

    void fillAnimFields() {
        if (anim_ >= int(def_.anims.size())) return;
        const psych::AnimDef& ad = def_.anims[size_t(anim_)];
        fName_ = ad.anim;
        fPrefix_ = ad.prefix;
        fFps_ = ad.fps;
        fLoop_ = ad.loop;
        fIndices_.clear();
        for (size_t i = 0; i < ad.indices.size(); ++i) fIndices_ += (i ? "," : "") + std::to_string(ad.indices[i]);
    }

    // Base positions as in a song: the player at the stage's (770, 100), the opponent at (100, 100).
    Vector2 basePos() const { return player_ ? Vector2{770, 100} : Vector2{100, 100}; }

    void rebuild() {
        error_.clear();
        ch_ = buildCharacter(sc_, def_, player_, error_);
        ghost_.reset();
        sc_.order.clear();
        prefixes_.clear();
        if (!ch_) return;
        const Vector2 b = basePos();
        ch_->x = b.x + def_.posX;
        ch_->y = b.y + def_.posY;
        if (ch_->graphic) prefixes_ = atlasPrefixes(*ch_->graphic);
        // The ghost: the first animation's first frame, faint, to line the others up against.
        ghost_ = buildCharacter(sc_, def_, player_, error_);
        if (ghost_) {
            ghost_->x = ch_->x;
            ghost_->y = ch_->y;
            ghost_->alpha = 0.33f;
            ghost_->paused = true;
            ghost_->visible = ghostOn_;
            sc_.order.push_back(ghost_);
        }
        sc_.order.push_back(ch_);
        playSelected();
    }

    void resetView() {
        if (ch_) {
            view_.x = ch_->x + ch_->width * 0.5f;
            view_.y = ch_->y + ch_->height * 0.5f;
        } else {
            view_.x = 640;
            view_.y = 360;
        }
        view_.zoom = 1;
        if (!app().touchMode || panelOpen_) view_.x -= (panelX() - gfx::W) * 0.5f / view_.zoom;  // centred left of the panel
    }

    void selectAnim(int i) {
        anim_ = clampv(i, 0, std::max(0, int(def_.anims.size()) - 1));
        fillAnimFields();
        playSelected();
    }

    void playSelected() {
        if (!ch_ || anim_ >= int(def_.anims.size())) return;
        ch_->playAnim(def_.anims[size_t(anim_)].anim, true);
        if (ghost_) ghost_->visible = ghostOn_;
    }

    void offsetsChanged() {
        dirty_ = true;
        leaveArmed_ = false;
        if (!ch_ || anim_ >= int(def_.anims.size())) return;
        const psych::AnimDef& ad = def_.anims[size_t(anim_)];
        for (scene::CharacterPtr c : {ch_, ghost_}) {
            if (!c) continue;
            if (scene::Anim* a = c->anim(ad.anim)) {
                a->offX = ad.offX;
                a->offY = ad.offY;
            }
            if (c->curAnimName() == ad.anim) {
                c->offsetX = ad.offX;
                c->offsetY = ad.offY;
            }
        }
    }

    void changed() {
        dirty_ = true;
        leaveArmed_ = false;
        rebuild();
    }

    bool drawInfo() {
        std::string l1 = "Animation: none";
        if (anim_ < int(def_.anims.size())) {
            const psych::AnimDef& ad = def_.anims[size_t(anim_)];
            l1 = "Anim: " + ad.anim + "   offset [" + jnum(ad.offX) + ", " + jnum(ad.offY) + "]";
            if (ch_ && !ch_->hasAnim(ad.anim)) l1 += "   (missing \"" + ad.prefix + "\")";
        }
        if (!app().touchMode) {
            DrawRectangleRec({8, 8, 856, 56}, fade(BLACK, 0.55f));
            gfx::text(FontId::Ui, l1, 18, 13, 20, kInk);
            gfx::text(FontId::Ui, saveName_ + (dirty_ ? "  (not saved)" : "") + "   mod: " + modLabel(mod_), 18, 38, 16, kDim);
            hints("W/S animation  SPACE play  ARROWS offset (SHIFT x10)  G ghost  RIGHT-DRAG move  WHEEL zoom  R reset  CTRL+S save");
            return false;
        }
        // Mobile top bar
        DrawRectangleRec({0, 0, gfx::W, 48}, Color{8, 5, 14, 242});
        DrawRectangleRec({0, 47, gfx::W, 1}, Color{110, 92, 150, 140});
        if (ui_.button({8, 6, 94, 36}, "< BACK")) {
            if (tryLeave()) return true;
        }
        gfx::text(FontId::Ui, l1, 114, 7, 18, kInk);
        gfx::text(FontId::Ui, saveName_ + (dirty_ ? " *" : "") + "   mod: " + modLabel(mod_), 114, 27, 14, kDim);
        if (ui_.button({gfx::W - 224, 6, 100, 36}, dirty_ ? "SAVE *" : "SAVE", dirty_)) save();
        if (ui_.button({gfx::W - 118, 6, 110, 36}, panelOpen_ ? "PANEL >" : "< PANEL", panelOpen_)) panelOpen_ = !panelOpen_;
        return false;
    }

    void drawTouchControls() {
        // Bottom-left compact offset nudge pad
        const Rectangle pad{12, gfx::H - 220, 174, 154};
        DrawRectangleRounded(pad, 0.08f, 6, Color{12, 8, 22, 205});
        DrawRectangleRoundedLinesEx(pad, 0.08f, 6, 1.5f, Color{110, 92, 150, 150});
        gfx::text(FontId::Ui, "OFFSET", pad.x + 10, pad.y + 9, 15, kAccent);
        if (ui_.button({pad.x + pad.width - 66, pad.y + 4, 60, 28}, nudgeBig_ ? "x10" : "x1", nudgeBig_)) nudgeBig_ = !nudgeBig_;
        auto nudge = [&](int dx, int dy) {
            if (anim_ < int(def_.anims.size())) {
                const int s = nudgeBig_ ? 10 : 1;
                def_.anims[size_t(anim_)].offX += float(dx * s);
                def_.anims[size_t(anim_)].offY += float(dy * s);
                offsetsChanged();
            }
        };
        if (ui_.button({pad.x + 62, pad.y + 38, 50, 36}, "^")) nudge(0, 1);
        if (ui_.button({pad.x + 8, pad.y + 76, 50, 36}, "<")) nudge(1, 0);
        if (ui_.button({pad.x + 116, pad.y + 76, 50, 36}, ">")) nudge(-1, 0);
        if (ui_.button({pad.x + 62, pad.y + 112, 50, 36}, "v")) nudge(0, -1);

        // Bottom transport & view bar
        const float by = gfx::H - 58.0f;
        DrawRectangleRec({0, by, gfx::W, 58}, Color{10, 7, 18, 248});
        DrawRectangleRec({0, by, gfx::W, 1.5f}, Color{110, 92, 150, 170});
        const float y = by + 7.0f, h = 44.0f;
        const int n = int(def_.anims.size());
        if (ui_.button({12, y, 118, h}, "< ANIM") && n > 0) selectAnim((anim_ + n - 1) % n);
        if (ui_.button({138, y, 110, h}, "PLAY")) playSelected();
        if (ui_.button({256, y, 118, h}, "ANIM >") && n > 0) selectAnim((anim_ + 1) % n);
        if (ui_.button({386, y, 112, h}, "GHOST", ghostOn_)) {
            ghostOn_ = !ghostOn_;
            if (ghost_) ghost_->visible = ghostOn_;
        }
        if (ui_.button({gfx::W - 348, y, 104, h}, "ZOOM -")) view_.zoom = clampv(view_.zoom / 1.2f, 0.05f, 8.0f);
        if (ui_.button({gfx::W - 236, y, 104, h}, "ZOOM +")) view_.zoom = clampv(view_.zoom * 1.2f, 0.05f, 8.0f);
        if (ui_.button({gfx::W - 124, y, 112, h}, "RESET")) resetView();
    }

    void drawPanel() {
        const bool touch = app().touchMode;
        if (touch && !panelOpen_) return;
        if (ui_.tap() && ui_.askId.empty()) ui_.focus.clear();
        const float px = panelX(), pw = panelW();
        const float topY = touch ? 54.0f : 8.0f;
        const float botY = touch ? (gfx::H - 64.0f) : (gfx::H - 40.0f);
        panelBox(topY, botY, px);
        const float x = px, w = pw, rh = touch ? 36.0f : 30.0f;
        float y = topY + 8.0f;
        if (ui_.button({x, y, w * 0.5f - 4, rh}, "Character", tab_ == 0)) { tab_ = 0; panelScroll_ = 0; }
        if (ui_.button({x + w * 0.5f + 4, y, w * 0.5f - 4, rh}, "Animations", tab_ == 1)) { tab_ = 1; panelScroll_ = 0; }
        y += rh + 12;
        const Rectangle viewRect{x - 4, y, w + 8, botY - y - 6};
        if (touch) ui_.beginScroll("char_panel", viewRect, panelScroll_);
        const float startY = touch ? (y - panelScroll_) : y;
        float curY = startY;
        if (tab_ == 0) curY = drawCharacterTab(x, startY, w, rh);
        else curY = drawAnimTab(x, startY, w, rh);
        if (touch) ui_.endScroll(viewRect, panelScroll_, curY - startY + 14.0f);
        ui_.overlays();
    }

    float drawCharacterTab(float x, float y, float w, float rh) {
        if (!names_.empty()) {
            if (ui_.dropdown("char", {x, y, w, rh}, "Character", names_, nameIdx_)) load(names_[size_t(nameIdx_)]);
            y += rh + 6;
        }
        ui_.text("save", {x, y, w, rh}, "Save as", saveName_);
        y += rh + 6;
        ui_.text("image", {x, y, w - 92, rh}, "Image", image_);
        if (ui_.button({x + w - 86, y, 86, rh}, "Reload")) {
            def_.images.clear();
            for (std::string part; const char ch : image_ + ",") {
                if (ch == ',') {
                    if (!trim(part).empty()) def_.images.push_back(trim(part));
                    part.clear();
                } else {
                    part += ch;
                }
            }
            changed();
        }
        y += rh + 6;
        if (ui_.text("icon", {x, y, w - 92, rh}, "Health icon", icon_)) {
            def_.icon = trim(icon_);
            dirty_ = true;
        }
        const std::string ip = iconPath(sc_, def_.icon);
        if (Texture2D* t = ip.empty() ? nullptr : rend_.texture(ip)) {
            const float fw = t->width >= t->height * 2 ? t->width / 2.0f : float(t->width);
            DrawTexturePro(*t, {0, 0, fw, float(t->height)}, {x + w - 80, y - 14, 58, 58 * t->height / fw}, {0, 0}, 0, WHITE);
        }
        y += rh + 22;
        double v = def_.scale;
        if (ui_.stepper({x, y, w, rh}, "Scale", v, 0.05, 0.05, 20, "%.2f")) {
            def_.scale = v;
            changed();
        }
        y += rh + 6;
        bool flip = def_.flipX, noAa = !def_.antialiasing;
        if (ui_.check({x, y, w * 0.5f, rh}, "Flip X", flip)) {
            def_.flipX = flip;
            changed();
        }
        if (ui_.check({x + w * 0.5f, y, w * 0.5f, rh}, "No antialiasing", noAa)) {
            def_.antialiasing = !noAa;
            changed();
        }
        y += rh + 6;
        auto posStep = [&](const char* label, float& f) {
            double d = f;
            if (ui_.stepper({x, y, w, rh}, label, d, 1, -10000, 10000, "%.0f")) {
                f = float(d);
                changed();
            }
            y += rh + 6;
        };
        posStep("Position X", def_.posX);
        posStep("Position Y", def_.posY);
        posStep("Camera X", def_.camX);
        posStep("Camera Y", def_.camY);
        double sd = def_.singDuration;
        if (ui_.stepper({x, y, w, rh}, "Sing duration", sd, 0.1, 0, 100, "%.1f")) {
            def_.singDuration = sd;
            dirty_ = true;
        }
        y += rh + 6;
        // Health bar colour: red, green, blue, and the icon's own colour.
        double rgb[3] = {double(def_.barColor >> 16 & 255), double(def_.barColor >> 8 & 255), double(def_.barColor & 255)};
        static const char* names[3] = {"Bar red", "Bar green", "Bar blue"};
        for (int i = 0; i < 3; ++i) {
            if (ui_.stepper({x, y, w - 46, rh}, names[i], rgb[i], 1, 0, 255, "%.0f")) {
                def_.barColor = uint32_t(rgb[0]) << 16 | uint32_t(rgb[1]) << 8 | uint32_t(rgb[2]);
                dirty_ = true;
            }
            y += rh + 4;
        }
        DrawRectangleRounded({x + w - 40, y - 3 * (rh + 4), 40, 3 * rh + 8}, 0.2f, 4, gfx::rgb(def_.barColor));
        if (ui_.button({x, y, w * 0.5f - 4, rh}, "Colour from icon")) {
            uint32_t c = 0;
            if (!ip.empty() && iconColor(ip, c)) {
                def_.barColor = c;
                dirty_ = true;
            } else {
                status_.set("No icon to take a colour from", true);
            }
        }
        bool pl = player_;
        if (ui_.check({x + w * 0.5f + 4, y, w * 0.5f, rh}, "As the player", pl)) {
            player_ = pl;
            rebuild();
        }
        y += rh + 10;
        if (ui_.button({x, y, w * 0.5f - 4, rh + 4}, "Save")) save();
        if (ui_.button({x + w * 0.5f + 4, y, w * 0.5f - 4, rh + 4}, "New character")) newCharacter();
        return y + rh + 10;
    }

    float drawAnimTab(float x, float y, float w, float rh) {
        // The animations: a click/tap picks and plays one.
        const bool touch = app().touchMode;
        const int n = int(def_.anims.size());
        const float row = touch ? 32.0f : 24.0f;
        const float listH = touch ? std::max(60.0f, float(n) * row + 6.0f) : 210.0f;
        DrawRectangleRec({x, y, w, listH}, Color{20, 14, 32, 255});
        const int rows = std::max(1, int(listH / row));
        if (CheckCollisionPointRec(ui_.mouse, {x, y, w, listH}) && ui_.wheel != 0) {
            listScroll_ = clampv(listScroll_ - ui_.wheel * 2, 0.0f, float(std::max(0, n - rows)));
            ui_.used = true;
        }
        const int first = touch ? 0 : int(listScroll_);
        const int count = touch ? n : std::min(rows, n - first);
        for (int k = 0; k < count && first + k < n; ++k) {
            const int i = first + k;
            const Rectangle r{x + 2, y + 2 + k * row, w - 4, row - 2};
            const bool hov = ui_.hover(r);
            if (i == anim_ || hov) DrawRectangleRec(r, i == anim_ ? Color{90, 66, 140, 255} : Color{50, 38, 76, 255});
            const psych::AnimDef& ad = def_.anims[size_t(i)];
            const bool ok = ch_ && ch_->hasAnim(ad.anim);
            gfx::text(FontId::Ui, ad.anim, r.x + 6, r.y + (r.height - 18) * 0.5f, 18, ok ? kInk : Color{255, 130, 130, 255});
            gfx::text(FontId::Ui, "[" + jnum(ad.offX) + ", " + jnum(ad.offY) + "]", r.x + r.width - 8, r.y + (r.height - 16) * 0.5f, 16, kDim, Align::Right);
            if (hov && ui_.tap()) {
                ui_.used = true;
                selectAnim(i);
            }
        }
        y += (touch ? (float(n) * row + 12.0f) : (listH + 10.0f));
        ui_.text("aname", {x, y, w, rh}, "Name", fName_);
        y += rh + 6;
        ui_.text("aprefix", {x, y, w, rh}, "Prefix", fPrefix_);
        y += rh + 6;
        if (!prefixes_.empty()) {
            if (ui_.dropdown("prefixes", {x, y, w, rh}, "In image", prefixes_, prefixIdx_)) fPrefix_ = prefixes_[size_t(prefixIdx_)];
            y += rh + 6;
        }
        ui_.stepper({x, y, w, rh}, "FPS", fFps_, 1, 1, 240, "%.0f");
        y += rh + 6;
        ui_.check({x, y, w * 0.4f, rh}, "Loop", fLoop_);
        ui_.text("aind", {x + w * 0.4f, y, w * 0.6f, rh}, "Indices", fIndices_);
        y += rh + 10;
        if (ui_.button({x, y, w * 0.5f - 4, rh}, "Add / update")) addOrUpdate();
        if (ui_.button({x + w * 0.5f + 4, y, w * 0.5f - 4, rh}, "Remove")) removeAnim();
        y += rh + 8;
        if (anim_ < n) {
            psych::AnimDef& ad = def_.anims[size_t(anim_)];
            double ox = ad.offX, oy = ad.offY;
            if (ui_.stepper({x, y, w, rh}, "Offset X", ox, 1, -5000, 5000, "%.0f")) {
                ad.offX = float(ox);
                offsetsChanged();
            }
            y += rh + 6;
            if (ui_.stepper({x, y, w, rh}, "Offset Y", oy, 1, -5000, 5000, "%.0f")) {
                ad.offY = float(oy);
                offsetsChanged();
            }
            y += rh + 6;
        }
        bool g = ghostOn_;
        if (ui_.check({x, y, w, rh}, "Ghost (first animation)", g)) {
            ghostOn_ = g;
            if (ghost_) ghost_->visible = g;
        }
        return y + rh + 8;
    }

    void addOrUpdate() {
        const std::string name = trim(fName_);
        if (name.empty() || trim(fPrefix_).empty()) {
            status_.set("An animation needs a name and a prefix", true);
            return;
        }
        psych::AnimDef ad;
        ad.anim = name;
        ad.prefix = trim(fPrefix_);
        ad.fps = fFps_;
        ad.loop = fLoop_;
        std::string part;
        for (char c : fIndices_ + ",") {
            if (c == ',') {
                if (!trim(part).empty()) ad.indices.push_back(std::atoi(trim(part).c_str()));
                part.clear();
            } else {
                part += c;
            }
        }
        auto it = std::find_if(def_.anims.begin(), def_.anims.end(), [&](const psych::AnimDef& a) { return a.anim == name; });
        if (it != def_.anims.end()) {
            ad.offX = it->offX;
            ad.offY = it->offY;
            *it = ad;
            anim_ = int(it - def_.anims.begin());
        } else {
            def_.anims.push_back(ad);
            anim_ = int(def_.anims.size()) - 1;
        }
        changed();
        if (ch_ && !ch_->hasAnim(name)) status_.set("No frames called \"" + ad.prefix + "\" in the image", true);
    }

    void removeAnim() {
        if (anim_ >= int(def_.anims.size())) return;
        def_.anims.erase(def_.anims.begin() + anim_);
        anim_ = std::max(0, anim_ - 1);
        fillAnimFields();
        changed();
    }

    void newCharacter() {
        def_ = psych::CharacterDef{};
        def_.images = {"characters/BOYFRIEND"};
        def_.icon = "face";
        psych::AnimDef idle;
        idle.anim = "idle";
        idle.prefix = "BF idle dance";
        def_.anims.push_back(idle);
        loadedPath_.clear();
        loadedText_.clear();
        saveName_ = "new-character";
        anim_ = 0;
        syncFields();
        changed();
        resetView();
    }

    void save() {
        const std::string name = fileName(saveName_);
        if (name.empty()) {
            status_.set("Type a name to save as", true);
            return;
        }
        std::string anims = "[\n";
        for (size_t i = 0; i < def_.anims.size(); ++i) {
            const psych::AnimDef& ad = def_.anims[i];
            std::string ind = "[";
            for (size_t k = 0; k < ad.indices.size(); ++k) ind += (k ? ", " : "") + std::to_string(ad.indices[k]);
            ind += "]";
            anims += "\t\t{\n\t\t\t\"anim\": " + jsonStr(ad.anim) + ",\n\t\t\t\"name\": " + jsonStr(ad.prefix) + ",\n\t\t\t\"fps\": " +
                     jnum(ad.fps) + ",\n\t\t\t\"loop\": " + (ad.loop ? "true" : "false") + ",\n\t\t\t\"indices\": " + ind +
                     ",\n\t\t\t\"offsets\": " + pair(ad.offX, ad.offY) + "\n\t\t}" + (i + 1 < def_.anims.size() ? ",\n" : "\n");
        }
        anims += "\t]";
        const uint32_t c = def_.barColor;
        const std::vector<std::pair<std::string, std::string>> fields = {
            {"animations", anims},
            {"image", jsonStr(imageKeys(def_))},
            {"scale", jnum(def_.scale)},
            {"sing_duration", jnum(def_.singDuration)},
            {"healthicon", jsonStr(def_.icon)},
            {"position", pair(def_.posX, def_.posY)},
            {"camera_position", pair(def_.camX, def_.camY)},
            {"flip_x", def_.flipX ? "true" : "false"},
            {"no_antialiasing", def_.antialiasing ? "false" : "true"},
            {"healthbar_colors", "[" + std::to_string(c >> 16 & 255) + ", " + std::to_string(c >> 8 & 255) + ", " + std::to_string(c & 255) + "]"},
            {"_editor_isPlayer", player_ ? "true" : "false"},
        };
        // The same file when it is in this mod; a new one in the mod otherwise.
        const std::string path = mod_.roots[0] + "characters/" + name + ".json";
        std::string err;
        if (!saveText(path, writeObject(fields, loadedText_), err)) {
            status_.set(err, true);
            return;
        }
        scene::readFile(path, loadedText_);
        loadedPath_ = path;
        dirty_ = leaveArmed_ = false;
        if (std::find(names_.begin(), names_.end(), name) == names_.end()) names_.push_back(name);
        status_.set("Saved " + path);
    }
};

// =========================================================== stage editor ==

class StageEditorState : public State {
public:
    void enter() override {
        mod_ = currentMod();
        panelOpen_ = !app().touchMode;
        scene::Scene probe;
        useMod(probe, mod_);
        names_ = listNames(probe, "stages/", ".json");
        auto it = std::find(names_.begin(), names_.end(), "stage");
        load(it != names_.end() ? *it : names_.empty() ? std::string("stage") : names_[0]);
    }
    bool keyReserved(int) const override { return !ui_.focus.empty(); }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        return t;
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (sc_) runStage(dt);
        const bool blocked = overPanel(ui_, !a.touchMode || panelOpen_, panelX()) ||
                             (a.touchMode && (ui_.mouse.y < 52.0f || ui_.mouse.y > gfx::H - 62.0f));
        view_.handle(in, ui_, blocked, a.touchMode && panMode_);
        if (!(a.touchMode && (panMode_ || touchPoints().size() >= 2))) {
            if (tab_ == 4) dragObjects(in, blocked);
            else dragCharacters(in, blocked);
        }
        if (!ui_.focus.empty()) return;
        const bool ctrl = in.isDown(KEY_LEFT_CONTROL) || in.isDown(KEY_RIGHT_CONTROL);
        if (in.pressed(KEY_ESCAPE)) {
            tryLeave();
            return;
        }
        if (ctrl && in.pressed(KEY_S)) save();
        if (in.pressed(KEY_R) && !ctrl) resetView();
        if (in.pressed(KEY_C)) showCam_ = !showCam_;
    }

    void draw() override {
        if (sc_) rend_.prepareShaderLayers(*sc_);
        gfx::beginCanvas();
        drawGrid(view_);
        if (sc_) {
            view_.apply(sc_->camGame);
            if (sc_->camGame.filters.empty()) {
                rend_.drawCamera(*sc_, scene::Cam::Game, nullptr, nullptr);
            } else {
                // The stage's camera shaders, as in a song: the camera to a
                // texture, then through them.
                gfx::endCanvas();
                const float cs = gfx::canvasScale();
                const Vector2 origin = gfx::canvasOrigin();
                const int rw = std::max(1, int(gfx::W * cs)), rh = std::max(1, int(gfx::H * cs));
                if (!camRt_.id || camRt_.texture.width != rw || camRt_.texture.height != rh) {
                    if (camRt_.id) UnloadRenderTexture(camRt_);
                    camRt_ = LoadRenderTexture(rw, rh);
                }
                gfx::beginTexture(camRt_);
                ClearBackground(BLANK);
                gfx::setRenderTargetSize(rw, rh);
                gfx::beginCanvas();
                rend_.drawCamera(*sc_, scene::Cam::Game, nullptr, nullptr);
                gfx::endCanvas();
                gfx::setRenderTargetSize(0, 0);
                gfx::endTexture();
                rend_.drawFiltered(*sc_, sc_->camGame.filters, camRt_, Rectangle{origin.x, origin.y, float(rw), float(rh)});
                gfx::beginCanvas();
            }
            static const Color cols[3] = {Color{120, 200, 255, 255}, Color{255, 140, 200, 255}, Color{200, 140, 255, 255}};
            for (int i = 0; i < 3; ++i) {
                if (i == 1 && def_.hideGf) continue;
                const float* p = pos(i);
                cross(view_.toCanvas(p[0], p[1]), 10, cols[i]);
                gfx::text(FontId::Ui, kWho[i], view_.toCanvas(p[0], p[1]).x + 12, view_.toCanvas(p[0], p[1]).y + 4, 16, cols[i]);
                if (const scene::CharacterPtr& c = chars_[i]; c && i == tab_ - 1) {
                    const Vector2 a = view_.toCanvas(c->x, c->y), b = view_.toCanvas(c->x + c->width, c->y + c->height);
                    DrawRectangleLinesEx({a.x, a.y, b.x - a.x, b.y - a.y}, 1.5f, cols[i]);
                }
            }
            if (showCam_) drawCameraFrame();
        }
        if (!app().touchMode) {
            DrawRectangleRec({8, 8, 856, 32}, fade(BLACK, 0.55f));
            gfx::text(FontId::Ui, saveName_ + (dirty_ ? "  (not saved)" : "") + "   mod: " + modLabel(mod_) + "   zoom " + jnum(def_.zoom), 18, 13,
                      18, kInk);
            hints("LEFT-DRAG a character to move it   RIGHT-DRAG move the view   WHEEL zoom   C camera   R reset   CTRL+S save");
        } else {
            DrawRectangleRec({0, 0, gfx::W, 48}, Color{8, 5, 14, 242});
            DrawRectangleRec({0, 47, gfx::W, 1}, Color{110, 92, 150, 140});
            if (ui_.button({8, 6, 94, 36}, "< BACK")) {
                if (tryLeave()) {
                    gfx::endCanvas();
                    return;
                }
            }
            gfx::text(FontId::Ui, saveName_ + (dirty_ ? " *" : "") + "   mod: " + modLabel(mod_) + "   zoom " + jnum(def_.zoom), 114, 14, 18, kInk);
            if (ui_.button({gfx::W - 224, 6, 100, 36}, dirty_ ? "SAVE *" : "SAVE", dirty_)) save();
            if (ui_.button({gfx::W - 118, 6, 110, 36}, panelOpen_ ? "PANEL >" : "< PANEL", panelOpen_)) panelOpen_ = !panelOpen_;
            drawTouchBar();
        }
        drawPanel();
        status_.draw();
        gfx::endCanvas();
    }

    ~StageEditorState() override {
        if (camRt_.id && IsWindowReady()) UnloadRenderTexture(camRt_);
    }

private:
    // The stage runs as in a song (at 100 BPM): tweens, timers, animations,
    // shaders, and its script's onUpdate, onStepHit and onBeatHit; the
    // characters dance on the beat.
    void runStage(double dt) {
        scene::Scene& sc = *sc_;
        const double stepMs = 60000.0 / kPreviewBpm / 4.0;
        const int stepBefore = int(std::floor(game_.clockMs / stepMs));
        game_.clockMs += dt * 1000.0;
        const float sx = sc.camGame.scrollX, sy = sc.camGame.scrollY, z = sc.camGame.zoom;
        sc.update(dt, stepMs, false);
        sc.camGame.scrollX = sx;  // the view is the editor's
        sc.camGame.scrollY = sy;
        sc.camGame.zoom = z;
        if (!host_) return;
        host_->setGlobal("songPosition", game_.clockMs);
        host_->call("onUpdate", {scene::Value(dt)});
        const int stepNow = int(std::floor(game_.clockMs / stepMs));
        for (int st = stepBefore + 1; st <= stepNow && st <= stepBefore + 4; ++st) {
            host_->setGlobal("curStep", double(st));
            host_->setGlobal("curBeat", double(st / 4));
            host_->call("onStepHit");
            if (st % 4 == 0) {
                for (const scene::CharacterPtr& c : chars_)
                    if (c) c->dance();
                host_->call("onBeatHit");
            }
        }
        host_->call("onUpdatePost", {scene::Value(dt)});
        host_->dispatchCompletions();
    }

    static constexpr double kPreviewBpm = 100;
    static constexpr const char* kWho[3] = {"Boyfriend", "Girlfriend", "Opponent"};
    std::unique_ptr<scene::Scene> sc_;
    std::unique_ptr<ScriptHost> host_;
    EditorGame game_;
    RenderTexture2D camRt_{};
    SceneRenderer rend_;
    Ui ui_;
    View view_;
    Status status_;
    ModChoice mod_;
    std::vector<std::string> names_;
    int nameIdx_ = 0, tab_ = 0, camWho_ = 0;
    psych::StageDef def_;
    std::string loadedText_, saveName_;
    std::string who_[3] = {"bf", "gf", "dad"};
    scene::CharacterPtr chars_[3];
    std::vector<scene::SpritePtr> objSprites_;  // per stage object (null for the characters' places)
    int objSel_ = 0;
    bool objDrag_ = false;
    bool dirty_ = false, leaveArmed_ = false, showCam_ = true;
    int drag_ = -1;
    Vector2 dragLast_{};
    bool panelOpen_ = true, panMode_ = false;
    float panelScroll_ = 0.0f;

    float panelX() const { return app().touchMode ? 836.0f : kPanelX; }
    float panelW() const { return gfx::W - panelX() - 8.0f; }

    bool tryLeave() {
        if (dirty_ && !leaveArmed_) {
            status_.set(app().touchMode ? "Unsaved changes: tap BACK again to leave, or tap SAVE"
                                        : "Unsaved changes: ESC again to leave without saving (CTRL+S saves)");
            leaveArmed_ = true;
            return false;
        }
        app().change(makeEditorMenu());
        return true;
    }

    float* pos(int i) { return i == 0 ? def_.bf : i == 1 ? def_.gf : def_.dad; }
    float* camOff(int i) { return i == 0 ? def_.camBf : i == 1 ? def_.camGf : def_.camDad; }

    void load(const std::string& name) {
        saveName_ = name;
        auto it = std::find(names_.begin(), names_.end(), name);
        nameIdx_ = it == names_.end() ? 0 : int(it - names_.begin());
        def_ = psych::StageDef{};
        loadedText_.clear();
        scene::Scene probe;
        useMod(probe, mod_);
        const std::string path = probe.files.find("stages/" + name + ".json");
        std::string err;
        if (!path.empty() && (!scene::readFile(path, loadedText_) || !psych::parseStage(loadedText_, def_, err)))
            status_.set("stages/" + name + ".json: " + err, true);
        dirty_ = leaveArmed_ = false;
        buildScene(name);
        resetView();
    }

    // The stage as a song shows it: its JSON objects, the three characters and
    // what its Lua script makes.
    void buildScene(const std::string& name) {
        host_.reset();
        rend_.clear();
        sc_ = std::make_unique<scene::Scene>();
        scene::Scene& sc = *sc_;
        useMod(sc, mod_);
        sc.stage = def_;
        sc.gfMarker = std::make_shared<scene::Sprite>();
        sc.gfMarker->kind = scene::Kind::Marker;
        sc.gfMarker->tag = "gfGroup";
        for (int i = 0; i < 3; ++i) makeChar(i);
        bool placed = false;
        objSprites_.clear();
        for (const psych::StageObject& o : def_.objects) {
            objSprites_.push_back(nullptr);
            const std::string t = toLower(o.type);
            if (t == "gf" || t == "gfgroup") {
                sc.order.push_back(sc.gfMarker);
                if (chars_[1]) sc.order.push_back(chars_[1]);
                placed = true;
                continue;
            }
            if (t == "dad" || t == "dadgroup") {
                if (chars_[2]) sc.order.push_back(chars_[2]);
                continue;
            }
            if (t == "boyfriend" || t == "boyfriendgroup") {
                if (chars_[0]) sc.order.push_back(chars_[0]);
                continue;
            }
            auto s = std::make_shared<scene::Sprite>();
            s->tag = o.name;
            if (t == "square") s->setGraphic(sc.gfx.solid(1, 1, 0xFF000000u | o.color));
            else if (t == "animatedsprite") s->setGraphic(sc.gfx.atlas(o.image));
            else s->setGraphic(sc.gfx.image(o.image));
            for (const psych::AnimDef& ad : o.anims) {
                if (!s->graphic) break;
                scene::Anim an;
                an.name = ad.anim;
                an.frames = ad.indices.empty() ? psych::framesByPrefix(s->graphic->frames, ad.prefix)
                                               : psych::framesByIndices(s->graphic->frames, ad.prefix, ad.indices);
                an.fps = ad.fps;
                an.loop = ad.loop;
                if (!an.frames.empty()) s->addAnim(std::move(an));
            }
            if (!o.firstAnim.empty()) s->play(o.firstAnim, true);
            s->x = o.x;
            s->y = o.y;
            s->scaleX = o.scaleX;
            s->scaleY = o.scaleY;
            if (t == "square") s->setGraphicSize(o.scaleX, o.scaleY), s->scaleX = o.scaleX, s->scaleY = o.scaleY;
            s->updateHitbox();
            s->scrollX = o.scrollX;
            s->scrollY = o.scrollY;
            s->alpha = o.alpha;
            s->angle = o.angle;
            s->color = o.color;
            s->flipX = o.flipX;
            s->flipY = o.flipY;
            s->antialiasing = o.antialiasing;
            s->inScene = true;
            if (!o.name.empty()) sc.objects[o.name] = s;
            sc.order.push_back(s);
            objSprites_.back() = s;
        }
        if (!placed) {
            sc.order.push_back(sc.gfMarker);
            if (chars_[1]) sc.order.push_back(chars_[1]);
            if (chars_[2]) sc.order.push_back(chars_[2]);
            if (chars_[0]) sc.order.push_back(chars_[0]);
        }
        // The stage's script builds its sprites (Psych 0.6 / 0.7 stages are Lua).
        const std::string lua = sc.files.find("stages/" + name + ".lua");
        game_.shaders = app().settings.shaders;
        game_.clockMs = 0;
        if (!lua.empty()) {
            host_ = std::make_unique<ScriptHost>(sc, game_);
            host_->load(lua);
            host_->call("onCreatePost");
        }
        rend_.preload(sc);
    }

    void makeChar(int i) {
        chars_[i].reset();
        scene::Scene& sc = *sc_;
        // Scripts reach them as boyfriend, gf and dad; the scene animates them.
        (i == 0 ? sc.bf : i == 1 ? sc.gf : sc.dad) = nullptr;
        if (i == 1 && def_.hideGf) return;
        psych::CharacterDef cd;
        std::string path, text, err;
        if (loadCharacterDef(sc, who_[i], cd, path, text, err)) chars_[i] = buildCharacter(sc, cd, i == 0, err);
        if (!chars_[i]) return;
        (i == 0 ? sc.bf : i == 1 ? sc.gf : sc.dad) = chars_[i];
        chars_[i]->scrollX = chars_[i]->scrollY = i == 1 ? 0.95f : 1.0f;
        place(i);
    }

    void place(int i) {
        if (!chars_[i]) return;
        const float* p = pos(i);
        chars_[i]->x = p[0] + chars_[i]->posX;
        chars_[i]->y = p[1] + chars_[i]->posY;
    }

    // Where the camera looks at character i, as Psych's moveCamera works it out.
    Vector2 cameraFocus(int i) {
        const scene::CharacterPtr& c = chars_[i];
        const float* off = camOff(i);
        if (!c) return {pos(i)[0] + off[0], pos(i)[1] + off[1]};
        const float mx = c->x + c->width * 0.5f, my = c->y + c->height * 0.5f;
        if (i == 0) return {mx - 100 - c->camX + off[0], my - 100 + c->camY + off[1]};
        if (i == 2) return {mx + 150 + c->camX + off[0], my - 100 + c->camY + off[1]};
        return {mx + c->camX + off[0], my + c->camY + off[1]};
    }

    void drawCameraFrame() {
        const Vector2 f = cameraFocus(camWho_);
        const float hw = gfx::W * 0.5f / float(std::max(0.05, def_.zoom)), hh = gfx::H * 0.5f / float(std::max(0.05, def_.zoom));
        const Vector2 a = view_.toCanvas(f.x - hw, f.y - hh), b = view_.toCanvas(f.x + hw, f.y + hh);
        DrawRectangleLinesEx({a.x, a.y, b.x - a.x, b.y - a.y}, 2, kGold);
        cross(view_.toCanvas(f.x, f.y), 14, kGold);
        gfx::text(FontId::Ui, std::string("Camera on ") + kWho[camWho_], a.x + 6, a.y + 4, 16, kGold);
    }

    void dragCharacters(const Input& in, bool blocked) {
        if (!sc_) return;
        const Vector2 w = view_.toWorld(ui_.mouse);
        if (in.mousePressed(MOUSE_BUTTON_LEFT) && !blocked && ui_.focus.empty()) {
            for (int i : {0, 2, 1}) {  // the front one first
                const scene::CharacterPtr& c = chars_[i];
                if (c && w.x >= c->x && w.x <= c->x + c->width && w.y >= c->y && w.y <= c->y + c->height) {
                    drag_ = i;
                    dragLast_ = w;
                    tab_ = i + 1;
                    camWho_ = i;
                    break;
                }
            }
        }
        if (drag_ >= 0) {
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                drag_ = -1;
                return;
            }
            float* p = pos(drag_);
            const float dx = std::round(w.x - dragLast_.x), dy = std::round(w.y - dragLast_.y);
            if (dx != 0 || dy != 0) {
                p[0] += dx;
                p[1] += dy;
                dragLast_.x += dx;
                dragLast_.y += dy;
                place(drag_);
                dirty_ = true;
                leaveArmed_ = false;
            }
        }
    }

    void resetView() {
        const Vector2 a = cameraFocus(2), b = cameraFocus(0);
        view_.x = (a.x + b.x) * 0.5f;
        view_.y = (a.y + b.y) * 0.5f;
        view_.zoom = float(std::max(0.05, def_.zoom)) * 0.55f;
        if (!app().touchMode || panelOpen_) view_.x -= (panelX() - gfx::W) * 0.5f / view_.zoom;
    }

    void drawTouchBar() {
        const float by = gfx::H - 58.0f;
        DrawRectangleRec({0, by, gfx::W, 58}, Color{10, 7, 18, 248});
        DrawRectangleRec({0, by, gfx::W, 1.5f}, Color{110, 92, 150, 170});
        const float y = by + 7.0f, h = 44.0f;
        if (ui_.button({12, y, 120, h}, "DRAG", !panMode_)) panMode_ = false;
        if (ui_.button({138, y, 120, h}, "PAN VIEW", panMode_)) panMode_ = true;
        if (ui_.button({270, y, 108, h}, "CAM BOX", showCam_)) showCam_ = !showCam_;
        if (ui_.button({390, y, 76, h}, "BF", camWho_ == 0)) { camWho_ = 0; tab_ = 1; }
        if (ui_.button({472, y, 76, h}, "GF", camWho_ == 1)) { camWho_ = 1; tab_ = 2; }
        if (ui_.button({554, y, 76, h}, "DAD", camWho_ == 2)) { camWho_ = 2; tab_ = 3; }
        if (ui_.button({gfx::W - 348, y, 104, h}, "ZOOM -")) view_.zoom = clampv(view_.zoom / 1.2f, 0.05f, 8.0f);
        if (ui_.button({gfx::W - 236, y, 104, h}, "ZOOM +")) view_.zoom = clampv(view_.zoom * 1.2f, 0.05f, 8.0f);
        if (ui_.button({gfx::W - 124, y, 112, h}, "RESET")) resetView();
    }

    void drawPanel() {
        const bool touch = app().touchMode;
        if (touch && !panelOpen_) return;
        if (ui_.tap() && ui_.askId.empty()) ui_.focus.clear();
        const float px = panelX(), pw = panelW();
        const float topY = touch ? 54.0f : 8.0f;
        const float botY = touch ? (gfx::H - 64.0f) : (gfx::H - 40.0f);
        panelBox(topY, botY, px);
        const float x = px, w = pw, rh = touch ? 36.0f : 30.0f;
        float y = topY + 8.0f;
        static const char* tabs[5] = {"Stage", "BF", "GF", "Dad", "Objects"};
        for (int i = 0; i < 5; ++i)
            if (ui_.button({x + i * (w / 5), y, w / 5 - 4, rh}, tabs[i], tab_ == i)) { tab_ = i; panelScroll_ = 0; }
        y += rh + 12;
        const Rectangle viewRect{x - 4, y, w + 8, botY - y - 6};
        if (touch) ui_.beginScroll("stage_panel", viewRect, panelScroll_);
        const float startY = touch ? (y - panelScroll_) : y;
        float curY = startY;
        if (tab_ == 0) curY = drawStageTab(x, startY, w, rh);
        else if (tab_ == 4) curY = drawObjectsTab(x, startY, w, rh);
        else curY = drawCharTab(tab_ - 1, x, startY, w, rh);
        if (touch) ui_.endScroll(viewRect, panelScroll_, curY - startY + 14.0f);
        ui_.overlays();
    }

    float drawStageTab(float x, float y, float w, float rh) {
        if (!names_.empty()) {
            if (ui_.dropdown("stage", {x, y, w, rh}, "Stage", names_, nameIdx_)) load(names_[size_t(nameIdx_)]);
            y += rh + 6;
        }
        ui_.text("save", {x, y, w, rh}, "Save as", saveName_);
        y += rh + 6;
        if (ui_.stepper({x, y, w, rh}, "Zoom", def_.zoom, 0.05, 0.05, 10, "%.2f")) dirty_ = true;
        y += rh + 6;
        if (ui_.stepper({x, y, w, rh}, "Camera speed", def_.camSpeed, 0.1, 0, 20, "%.1f")) dirty_ = true;
        y += rh + 6;
        if (ui_.check({x, y, w, rh}, "Pixel stage", def_.pixel)) dirty_ = true;
        y += rh + 6;
        if (ui_.check({x, y, w, rh}, "Hide girlfriend", def_.hideGf)) {
            dirty_ = true;
            buildScene(saveName_);
        }
        y += rh + 6;
        ui_.check({x, y, w * 0.5f, rh}, "Camera frame", showCam_);
        std::vector<std::string> who = {"BF", "GF", "Dad"};
        ui_.choice({x + w * 0.5f, y, w * 0.5f, rh}, "", who, camWho_, 0);
        y += rh + 12;
        gfx::text(FontId::Ui, def_.objects.empty() ? "Sprites: from the stage's Lua script" : TextFormat("%d objects in the JSON", int(def_.objects.size())),
                  x, y, 16, kDim);
        y += 26;
        if (ui_.button({x, y, w * 0.5f - 4, rh + 4}, "Save")) save();
        if (ui_.button({x + w * 0.5f + 4, y, w * 0.5f - 4, rh + 4}, "Reload")) load(saveName_);
        return y + rh + 12;
    }

    // Issue #22: the stage's own objects (Psych 0.7.3 "objects"): pick one
    // (here or by clicking it), drag it, change what it is and how it shows,
    // its layer, add sprites and remove them. Saved with the stage's JSON.
    float drawObjectsTab(float x, float y, float w, float rh) {
        std::vector<std::string> names;
        for (size_t i = 0; i < def_.objects.size(); ++i) names.push_back(objLabel(i));
        if (!names.empty()) {
            objSel_ = std::clamp(objSel_, 0, int(names.size()) - 1);
            ui_.dropdown("obj", {x, y, w, rh}, "Object", names, objSel_);
            y += rh + 8;
        } else {
            gfx::text(FontId::Ui, "No objects in the stage's JSON yet.", x, y, 16, kDim);
            y += 26;
        }
        auto changed = [&] {
            dirty_ = true;
            leaveArmed_ = false;
            applyObject(size_t(objSel_));
        };
        if (!names.empty()) {
            psych::StageObject& o = def_.objects[size_t(objSel_)];
            const std::string t = toLower(o.type);
            if (t == "gf" || t == "gfgroup" || t == "dad" || t == "dadgroup" || t == "boyfriend" || t == "boyfriendgroup") {
                gfx::text(FontId::Ui, "Where this character is drawn (its layer).", x, y, 16, kDim);
                y += 26;
            } else {
                ui_.text("objname", {x, y, w, rh}, "Name", o.name);
                y += rh + 6;
                std::string img = o.image;
                if (ui_.text("objimg", {x, y, w - 92, rh}, "Image", img)) o.image = img;
                if (ui_.button({x + w - 86, y, 86, rh}, "Load")) {
                    dirty_ = true;
                    buildScene(saveName_);
                }
                y += rh + 6;
                auto step = [&](const char* label, float& f, double inc, double lo, double hi, const char* fmt) {
                    double d = f;
                    if (ui_.stepper({x, y, w, rh}, label, d, inc, lo, hi, fmt)) {
                        f = float(d);
                        changed();
                    }
                    y += rh + 6;
                };
                step("X", o.x, 10, -100000, 100000, "%.0f");
                step("Y", o.y, 10, -100000, 100000, "%.0f");
                step("Scale X", o.scaleX, 0.05, -50, 50, "%.2f");
                step("Scale Y", o.scaleY, 0.05, -50, 50, "%.2f");
                step("Scroll X", o.scrollX, 0.05, -10, 10, "%.2f");
                step("Scroll Y", o.scrollY, 0.05, -10, 10, "%.2f");
                step("Alpha", o.alpha, 0.05, 0, 1, "%.2f");
                step("Angle", o.angle, 1, -3600, 3600, "%.0f");
                if (ui_.check({x, y, w / 3, rh}, "Flip X", o.flipX)) changed();
                if (ui_.check({x + w / 3, y, w / 3, rh}, "Flip Y", o.flipY)) changed();
                if (ui_.check({x + 2 * w / 3, y, w / 3, rh}, "Smooth", o.antialiasing)) changed();
                y += rh + 8;
            }
            const float bw = (w - 12) / 4;
            if (ui_.button({x, y, bw, rh}, "Back") && objSel_ > 0) moveObject(objSel_, objSel_ - 1);
            if (ui_.button({x + bw + 4, y, bw, rh}, "Front") && objSel_ + 1 < int(def_.objects.size())) moveObject(objSel_, objSel_ + 1);
            if (ui_.button({x + 2 * (bw + 4), y, bw, rh}, "Copy")) {
                psych::StageObject c = def_.objects[size_t(objSel_)];
                c.name += "_copy";
                c.x += 40;
                c.y += 40;
                def_.objects.insert(def_.objects.begin() + objSel_ + 1, c);
                ++objSel_;
                dirty_ = true;
                buildScene(saveName_);
            }
            if (ui_.button({x + 3 * (bw + 4), y, bw, rh}, "Delete")) {
                def_.objects.erase(def_.objects.begin() + objSel_);
                objSel_ = std::max(0, objSel_ - 1);
                dirty_ = true;
                buildScene(saveName_);
            }
            y += rh + 8;
        }
        if (ui_.button({x, y, w, rh}, "Add a sprite (an image of the mod)")) {
            psych::StageObject o;
            o.type = "sprite";
            o.name = "sprite" + std::to_string(def_.objects.size() + 1);
            o.image = def_.objects.empty() ? "stageback" : def_.objects[size_t(objSel_)].image;
            if (sc_) {  // where the view is looking
                const Vector2 c = view_.toWorld({gfx::W * 0.4f, gfx::H * 0.5f});
                o.x = std::round(c.x);
                o.y = std::round(c.y);
            }
            if (def_.objects.empty()) {  // the characters keep their places among the objects
                for (const char* who : {"gf", "dad", "boyfriend"}) {
                    psych::StageObject m;
                    m.type = who;
                    def_.objects.push_back(m);
                }
                def_.objects.insert(def_.objects.begin(), o);
                objSel_ = 0;
            } else {
                def_.objects.insert(def_.objects.begin() + objSel_ + 1, o);
                ++objSel_;
            }
            dirty_ = true;
            buildScene(saveName_);
        }
        y += rh + 6;
        gfx::text(FontId::Ui, app().touchMode ? "Tap an object to pick it; drag to move it." : "Click an object in the view to pick it; drag to move it.", x, y, 15, kDim);
        return y + 24;
    }

    std::string objLabel(size_t i) const {
        const psych::StageObject& o = def_.objects[i];
        const std::string t = toLower(o.type);
        if (t == "gf" || t == "gfgroup") return std::to_string(i) + ": (girlfriend)";
        if (t == "dad" || t == "dadgroup") return std::to_string(i) + ": (opponent)";
        if (t == "boyfriend" || t == "boyfriendgroup") return std::to_string(i) + ": (boyfriend)";
        return std::to_string(i) + ": " + (o.name.empty() ? o.image : o.name);
    }

    // The sprite on screen follows the object's numbers.
    void applyObject(size_t i) {
        if (i >= objSprites_.size() || !objSprites_[i]) return;
        const psych::StageObject& o = def_.objects[i];
        scene::Sprite& s = *objSprites_[i];
        s.x = o.x;
        s.y = o.y;
        s.scaleX = o.scaleX;
        s.scaleY = o.scaleY;
        if (toLower(o.type) == "square") s.setGraphicSize(o.scaleX, o.scaleY);
        s.updateHitbox();
        s.scrollX = o.scrollX;
        s.scrollY = o.scrollY;
        s.alpha = o.alpha;
        s.angle = o.angle;
        s.flipX = o.flipX;
        s.flipY = o.flipY;
        s.antialiasing = o.antialiasing;
    }
    void moveObject(int from, int to) {
        std::swap(def_.objects[size_t(from)], def_.objects[size_t(to)]);
        objSel_ = to;
        dirty_ = true;
        buildScene(saveName_);
    }

    // Objects: a click picks the front one under the mouse; a drag moves it.
    void dragObjects(const Input& in, bool blocked) {
        if (!sc_ || tab_ != 4) return;
        const Vector2 w = view_.toWorld(ui_.mouse);
        if (in.mousePressed(MOUSE_BUTTON_LEFT) && !blocked && ui_.focus.empty()) {
            for (int i = int(objSprites_.size()) - 1; i >= 0; --i) {
                const scene::SpritePtr& s = objSprites_[size_t(i)];
                if (!s) continue;
                const float sx = s->x - s->offsetX, sy = s->y - s->offsetY;
                if (w.x >= sx && w.x <= sx + s->width && w.y >= sy && w.y <= sy + s->height) {
                    objSel_ = i;
                    objDrag_ = true;
                    dragLast_ = w;
                    break;
                }
            }
        }
        if (!objDrag_) return;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            objDrag_ = false;
            return;
        }
        const float dx = std::round(w.x - dragLast_.x), dy = std::round(w.y - dragLast_.y);
        if ((dx != 0 || dy != 0) && objSel_ >= 0 && objSel_ < int(def_.objects.size())) {
            def_.objects[size_t(objSel_)].x += dx;
            def_.objects[size_t(objSel_)].y += dy;
            dragLast_.x += dx;
            dragLast_.y += dy;
            applyObject(size_t(objSel_));
            dirty_ = true;
            leaveArmed_ = false;
        }
    }

    // The objects as Psych 0.7.3 writes them.
    std::string objectsJson() const {
        std::string o = "[";
        for (size_t i = 0; i < def_.objects.size(); ++i) {
            const psych::StageObject& b = def_.objects[i];
            o += i ? ",\n\t\t{" : "\n\t\t{";
            o += "\"type\": " + jsonStr(b.type.empty() ? std::string("sprite") : b.type);
            const std::string t = toLower(b.type);
            if (t != "gf" && t != "gfgroup" && t != "dad" && t != "dadgroup" && t != "boyfriend" && t != "boyfriendgroup") {
                o += ", \"name\": " + jsonStr(b.name) + ", \"image\": " + jsonStr(b.image) + ", \"x\": " + jnum(b.x) +
                     ", \"y\": " + jnum(b.y) + ", \"scale\": " + pair(b.scaleX, b.scaleY) + ", \"scroll\": " + pair(b.scrollX, b.scrollY) +
                     ", \"alpha\": " + jnum(b.alpha) + ", \"angle\": " + jnum(b.angle) + ", \"color\": " +
                     jsonStr(TextFormat("#%06X", unsigned(b.color & 0xFFFFFF))) + ", \"flipX\": " + (b.flipX ? "true" : "false") +
                     ", \"flipY\": " + (b.flipY ? "true" : "false") + ", \"antialiasing\": " + (b.antialiasing ? "true" : "false");
                if (!b.anims.empty()) {
                    o += ", \"animations\": [";
                    for (size_t k = 0; k < b.anims.size(); ++k) {
                        const psych::AnimDef& a = b.anims[k];
                        std::string ind;
                        for (size_t q = 0; q < a.indices.size(); ++q) ind += (q ? ", " : "") + std::to_string(a.indices[q]);
                        o += std::string(k ? ", " : "") + "{\"anim\": " + jsonStr(a.anim) + ", \"name\": " + jsonStr(a.prefix) +
                             ", \"fps\": " + jnum(a.fps) + ", \"loop\": " + (a.loop ? "true" : "false") + ", \"indices\": [" + ind +
                             "], \"offsets\": " + pair(a.offX, a.offY) + "}";
                    }
                    o += "]";
                }
                if (!b.firstAnim.empty()) o += ", \"firstAnimation\": " + jsonStr(b.firstAnim);
            }
            o += "}";
        }
        return o + (def_.objects.empty() ? "]" : "\n\t]");
    }

    float drawCharTab(int i, float x, float y, float w, float rh) {
        gfx::text(FontId::Ui, kWho[i], x, y, 22, kGold);
        y += 34;
        ui_.text("who", {x, y, w - 92, rh}, "Character", who_[i]);
        if (ui_.button({x + w - 86, y, 86, rh}, "Show")) {
            who_[i] = trim(who_[i]);
            buildScene(saveName_);
        }
        y += rh + 6;
        gfx::text(FontId::Ui, "Only shown here: songs choose their characters.", x, y, 15, kDim);
        y += 26;
        float* p = pos(i);
        float* c = camOff(i);
        auto step = [&](const char* label, float& f, bool moves) {
            double d = f;
            if (ui_.stepper({x, y, w, rh}, label, d, 1, -20000, 20000, "%.0f")) {
                f = float(d);
                dirty_ = true;
                if (moves) place(i);
            }
            y += rh + 6;
        };
        step("Position X", p[0], true);
        step("Position Y", p[1], true);
        step("Camera X", c[0], false);
        step("Camera Y", c[1], false);
        camWho_ = i;
        return y + 8;
    }

    void save() {
        const std::string name = fileName(saveName_);
        if (name.empty()) {
            status_.set("Type a name to save as", true);
            return;
        }
        const std::vector<std::pair<std::string, std::string>> fields = {
            {"directory", jsonStr(def_.directory)},
            {"defaultZoom", jnum(def_.zoom)},
            {"isPixelStage", def_.pixel ? "true" : "false"},
            {"boyfriend", pair(def_.bf[0], def_.bf[1])},
            {"girlfriend", pair(def_.gf[0], def_.gf[1])},
            {"opponent", pair(def_.dad[0], def_.dad[1])},
            {"hide_girlfriend", def_.hideGf ? "true" : "false"},
            {"camera_boyfriend", pair(def_.camBf[0], def_.camBf[1])},
            {"camera_opponent", pair(def_.camDad[0], def_.camDad[1])},
            {"camera_girlfriend", pair(def_.camGf[0], def_.camGf[1])},
            {"camera_speed", jnum(def_.camSpeed)},
        };
        std::vector<std::pair<std::string, std::string>> all = fields;
        if (!def_.objects.empty()) all.push_back({"objects", objectsJson()});
        const std::string path = mod_.roots[0] + "stages/" + name + ".json";
        std::string err;
        if (!saveText(path, writeObject(all, loadedText_), err)) {
            status_.set(err, true);
            return;
        }
        scene::readFile(path, loadedText_);
        dirty_ = leaveArmed_ = false;
        if (std::find(names_.begin(), names_.end(), name) == names_.end()) names_.push_back(name);
        status_.set("Saved " + path);
    }
};

// =========================================================== event editor ==

struct EventTemplate {
    const char* name;
    const char* v1;
    const char* v2;
    const char* body;
};

const EventTemplate kTemplates[] = {
    {"Empty", "", "", "\t-- what the event does\n"},
    {"Camera flash", "Duration (s)", "Colour (FFFFFF)",
     "\tlocal colour = value2 ~= '' and value2 or 'FFFFFF'\n\tcameraFlash('game', colour, tonumber(value1) or 0.5, true)\n"},
    {"Fade the HUD", "Alpha (0 to 1)", "Duration (s)",
     "\tdoTweenAlpha('eventHudFade', 'camHUD', tonumber(value1) or 0, tonumber(value2) or 0.5, 'linear')\n"},
    {"Camera shake", "Intensity (0.01)", "Duration (s)",
     "\tcameraShake('game', tonumber(value1) or 0.01, tonumber(value2) or 0.3)\n\tcameraShake('hud', (tonumber(value1) or 0.01) / 2, tonumber(value2) or 0.3)\n"},
    {"Zoom the camera", "Zoom (0.9)", "Duration (s)",
     "\tdoTweenZoom('eventZoom', 'camGame', tonumber(value1) or 0.9, tonumber(value2) or 0.5, 'quadOut')\n\tsetProperty('defaultCamZoom', tonumber(value1) or 0.9)\n"},
    {"Play a sound", "Sound (sounds/...)", "Volume (0 to 1)", "\tplaySound(value1, tonumber(value2) or 1)\n"},
    {"Scroll speed", "Multiplier", "Duration (s)", "\ttriggerEvent('Change Scroll Speed', value1, value2)\n"},
};

class EventEditorState : public State {
public:
    void enter() override {
        mod_ = currentMod();
        useMod(sc_, mod_);
        refresh();
    }
    bool keyReserved(int) const override { return !ui_.focus.empty(); }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        return t;
    }

    void update(double) override {
        App& a = app();
        feedUi(ui_);
        if (!ui_.focus.empty()) return;
        if (a.input.pressed(KEY_ESCAPE)) a.change(makeEditorMenu());
    }

    void draw() override {
        App& a = app();
        const bool touch = a.touchMode;
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x6C8CFF, 0);
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, fade(BLACK, 0.45f));
        if (touch) {
            if (ui_.button({20, 18, 110, 42}, "< BACK")) {
                a.change(makeEditorMenu());
                gfx::endCanvas();
                return;
            }
            gfx::text(FontId::Title, "EVENT EDITOR", 148, 18, 38, kGold, Align::Left, 2.0f);
            gfx::text(FontId::Ui, "Mod: " + modLabel(mod_) + "   custom_events/<name>.txt and .lua", 148, 66, 17, kIvory);
        } else {
            gfx::text(FontId::Title, "EVENT EDITOR", 40, 18, 40, kGold, Align::Left, 2.0f);
            gfx::text(FontId::Ui, "Custom events of " + modLabel(mod_) + ": custom_events/<name>.txt (what the chart editor shows) and .lua",
                      40, 70, 18, kIvory);
        }
        if (ui_.tap() && ui_.askId.empty()) ui_.focus.clear();
        drawList();
        if (drawForm()) {
            gfx::endCanvas();
            return;
        }
        ui_.overlays();
        hints("Click an event to see it   ESC back");
        status_.draw();
        gfx::endCanvas();
    }

private:
    scene::Scene sc_;
    Ui ui_;
    Status status_;
    ModChoice mod_;
    std::vector<std::string> names_;
    int sel_ = -1, template_ = 0;
    float scroll_ = 0, formScroll_ = 0;
    std::string name_, desc_, v1_, v2_, script_;

    void refresh() {
        std::set<std::string> all;
        for (const char* ext : {".txt", ".lua"})
            for (const std::string& n : listNames(sc_, "custom_events/", ext)) all.insert(n);
        names_.assign(all.begin(), all.end());
    }

    std::string mine(const std::string& ext) const { return mod_.roots[0] + "custom_events/" + fileName(name_) + ext; }

    void open(int i) {
        sel_ = i;
        name_ = names_[size_t(i)];
        desc_.clear();
        script_.clear();
        std::string text;
        if (const std::string p = sc_.files.find("custom_events/" + name_ + ".txt"); !p.empty() && scene::readFile(p, text)) desc_ = trim(text);
        if (const std::string p = sc_.files.find("custom_events/" + name_ + ".lua"); !p.empty()) scene::readFile(p, script_);
    }

    void drawList() {
        const bool touch = app().touchMode;
        const Rectangle box{touch ? 20.0f : 40.0f, 100, touch ? 360.0f : 400.0f, gfx::H - (touch ? 118.0f : 160.0f)};
        DrawRectangleRounded(box, 0.02f, 6, kPanel);
        const int n = int(names_.size());
        if (n == 0) gfx::text(FontId::Ui, "No custom events yet", box.x + 12, box.y + 10, 18, kDim);
        if (!touch) {
            const float row = 26;
            const int rows = int((box.height - 8) / row);
            if (CheckCollisionPointRec(ui_.mouse, box) && ui_.wheel != 0) scroll_ = clampv(scroll_ - ui_.wheel * 3, 0.0f, float(std::max(0, n - rows)));
            const int first = int(scroll_);
            for (int k = 0; k < rows && first + k < n; ++k) {
                const int i = first + k;
                const Rectangle r{box.x + 4, box.y + 4 + k * row, box.width - 8, row - 2};
                const bool hov = ui_.hover(r);
                if (i == sel_ || hov) DrawRectangleRec(r, i == sel_ ? Color{90, 66, 140, 255} : Color{50, 38, 76, 255});
                gfx::text(FontId::Ui, names_[size_t(i)], r.x + 8, r.y + 3, 18, kInk);
                if (hov && ui_.click) {
                    ui_.used = true;
                    open(i);
                }
            }
            return;
        }
        const Rectangle view{box.x + 4, box.y + 4, box.width - 8, box.height - 8};
        const float row = 38.0f;
        ui_.beginScroll("ev_list", view, scroll_);
        for (int i = 0; i < n; ++i) {
            const float y = view.y + float(i) * row - scroll_;
            if (y + row < view.y || y > view.y + view.height) continue;
            const Rectangle r{view.x + 2, y + 2, view.width - 10, row - 4};
            const bool hov = ui_.hover(r);
            if (i == sel_ || hov) DrawRectangleRounded(r, 0.18f, 4, i == sel_ ? Color{90, 66, 140, 255} : Color{50, 38, 76, 255});
            gfx::text(FontId::Ui, names_[size_t(i)], r.x + 10, r.y + 8, 18, kInk);
            if (hov && ui_.tap()) {
                ui_.used = true;
                open(i);
            }
        }
        ui_.endScroll(view, scroll_, float(n) * row + 8.0f);
    }

    bool drawForm() {
        const bool touch = app().touchMode;
        const float x = touch ? 404.0f : 470.0f;
        const float w = gfx::W - x - (touch ? 24.0f : 40.0f);
        const float rh = touch ? 36.0f : 30.0f;
        const Rectangle box{x - 10, 96, w + 20, gfx::H - (touch ? 114.0f : 152.0f)};
        DrawRectangleRounded(box, 0.02f, 6, kPanel);
        const Rectangle view{box.x + 6, box.y + 6, box.width - 12, box.height - 12};
        if (touch) ui_.beginScroll("ev_form", view, formScroll_);
        const float startY = touch ? (view.y + 4.0f - formScroll_) : 104.0f;
        float y = startY;
        ui_.text("name", {x, y, w, rh}, "Name", name_);
        y += rh + 8;
        ui_.text("desc", {x, y, w, rh}, "Description", desc_);
        y += rh + 8;
        ui_.text("v1", {x, y, w, rh}, "Value 1", v1_);
        y += rh + 8;
        ui_.text("v2", {x, y, w, rh}, "Value 2", v2_);
        y += rh + 8;
        std::vector<std::string> tnames;
        for (const EventTemplate& t : kTemplates) tnames.push_back(t.name);
        if (ui_.dropdown("tmpl", {x, y, w, rh}, "Does", tnames, template_)) {
            const EventTemplate& t = kTemplates[template_];
            if (v1_.empty() || isTemplateHint(v1_)) v1_ = t.v1;
            if (v2_.empty() || isTemplateHint(v2_)) v2_ = t.v2;
        }
        y += rh + 12;
        if (!touch) {
            if (ui_.button({x, y, 220, rh + 4}, "Create / save")) create();
            if (ui_.button({x + 230, y, 160, rh + 4}, "Open script")) openFile(".lua");
            if (ui_.button({x + 400, y, 160, rh + 4}, "Open folder")) {
                std::error_code ec;
                fs::create_directories(toPath(mod_.roots[0] + "custom_events"), ec);
                if (!openWithSystem(mod_.roots[0] + "custom_events")) status_.set("This system cannot open folders from the game", true);
            }
        } else {
            const float bw = (w - 16.0f) * 0.5f;
            if (ui_.button({x, y, bw, rh + 6}, "Create / save")) create();
            if (ui_.button({x + bw + 16, y, bw, rh + 6}, "Edit in Script Editor")) {
                if (touch) ui_.endScroll(view, formScroll_, y - startY);
                app().change(makeScriptEditor());
                return true;
            }
        }
        y += rh + 20;
        gfx::text(FontId::Ui, "Script:", x, y, 18, kDim);
        y += 24;
        // The script as it is (the first lines).
        int line = 0;
        size_t p = 0;
        const std::string shown = script_.empty() ? scriptFor() : script_;
        while (p < shown.size() && line < 14) {
            size_t e = shown.find('\n', p);
            if (e == std::string::npos) e = shown.size();
            std::string l = shown.substr(p, e - p);
            for (size_t t; (t = l.find('\t')) != std::string::npos;) l.replace(t, 1, "    ");
            gfx::text(FontId::Ui, l, x, y, 16, script_.empty() ? kDim : kInk);
            y += 20;
            ++line;
            p = e + 1;
        }
        if (touch) ui_.endScroll(view, formScroll_, y - startY + 14.0f);
        return false;
    }

    static bool isTemplateHint(const std::string& s) {
        for (const EventTemplate& t : kTemplates)
            if (s == t.v1 || s == t.v2) return true;
        return false;
    }

    std::string scriptFor() const {
        const std::string n = trim(name_).empty() ? std::string("My Event") : trim(name_);
        std::string s = "-- " + n + (desc_.empty() ? "" : ": " + desc_) + "\n";
        if (!v1_.empty()) s += "-- Value 1: " + v1_ + "\n";
        if (!v2_.empty()) s += "-- Value 2: " + v2_ + "\n";
        std::string quoted;
        for (char c : n) quoted += c == '\'' ? std::string("\\'") : std::string(1, c);
        s += "function onEvent(name, value1, value2)\n\tif name ~= '" + quoted + "' then return end\n";
        s += kTemplates[template_].body;
        s += "end\n";
        return s;
    }

    void create() {
        const std::string n = fileName(name_);
        if (n.empty()) {
            status_.set("Type the event's name", true);
            return;
        }
        // The description file holds what the chart editor shows for it.
        std::string desc = desc_;
        if (!v1_.empty()) desc += "\nValue 1: " + v1_;
        if (!v2_.empty()) desc += "\nValue 2: " + v2_;
        std::string err;
        if (!saveText(mine(".txt"), desc + "\n", err)) {
            status_.set(err, true);
            return;
        }
        std::error_code ec;
        const bool hasScript = fs::exists(toPath(mine(".lua")), ec);
        if (!hasScript && !saveText(mine(".lua"), scriptFor(), err)) {
            status_.set(err, true);
            return;
        }
        refresh();
        auto it = std::find(names_.begin(), names_.end(), n);
        if (it != names_.end()) open(int(it - names_.begin()));
        status_.set(hasScript ? "Saved the description (the script was there: it is kept)" : "Made " + mine(".txt") + " and its .lua");
    }

    void openFile(const std::string& ext) {
        std::string p = sc_.files.find("custom_events/" + trim(name_) + ext);
        if (p.empty()) {
            status_.set("Create the event first", true);
            return;
        }
        if (!openWithSystem(p)) status_.set("This system cannot open files from the game: " + p, true);
    }
};

// ======================================================= SNIFF converter ==
// In-engine FL Studio (.flp / .fsc) chart converter with configurable MIDI
// note ranges for both opponent and player, supporting multikey charts (1-18K).

class SniffEditorState : public State {
public:
    bool keyReserved(int) const override { return !ui_.focus.empty(); }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        return t;
    }

    void enter() override {
        mod_ = currentMod();
        scanFiles();
        if (!files_.empty()) loadFile(files_[0]);
    }

    void update(double) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            for (unsigned i = 0; i < dropped.count; ++i) {
                const std::string f = dropped.paths[i];
                if (endsWithI(f, ".flp") || endsWithI(f, ".fsc")) {
                    loadFile(f);
                    break;
                }
            }
            UnloadDroppedFiles(dropped);
        }
        if (in.back() && ui_.focus.empty() && ui_.dropId.empty()) {
            sfxPlay(Sfx::Cancel);
            a.change(makeEditorMenu());
            return;
        }
    }

    void draw() override {
        App& a = app();
        const bool touch = a.touchMode;
        if (ui_.tap() && ui_.askId.empty()) ui_.focus.clear();
        gfx::beginCanvas();
        drawMenuBackground(a.now, 0x58B4FF, menuPulse());

        // Top bar
        const float topH = touch ? 50.0f : 46.0f;
        DrawRectangleRec({0, 0, gfx::W, topH}, Color{8, 5, 16, 240});
        DrawRectangleRec({0, topH - 1, gfx::W, 1}, Color{110, 92, 150, 150});
        if (ui_.button({12, 7, 104, topH - 14}, "< BACK")) {
            sfxPlay(Sfx::Cancel);
            a.change(makeEditorMenu());
            gfx::endCanvas();
            return;
        }
        gfx::text(FontId::Title, "SNIFF  (FLP / FSC CONVERTER)", 132, 10, touch ? 26.0f : 28.0f, kGold);
        if (ui_.button({gfx::W - 140, 7, 126, topH - 14}, "Rescan files")) {
            scanFiles();
            status_.set(TextFormat("Found %zu .flp/.fsc files", files_.size()));
        }

        const float bodyY = topH + 10.0f;
        const float bodyH = gfx::H - bodyY - (touch ? 12.0f : 48.0f);
        const float leftW = 540.0f;
        const float rightX = leftW + 28.0f;
        const float rightW = gfx::W - rightX - 16.0f;

        // ---- Left column: file path, discovered files, patterns
        DrawRectangleRounded({14, bodyY, leftW, bodyH}, 0.02f, 6, kPanel);
        DrawRectangleRoundedLinesEx({14, bodyY, leftW, bodyH}, 0.02f, 6, 1.5f, Color{110, 92, 150, 150});
        float ly = bodyY + 12.0f;
        const float lx = 26.0f, lw = leftW - 24.0f;
        const float rh = touch ? 36.0f : 30.0f;
        gfx::text(FontId::Ui, "1. Select or drop a .flp / .fsc file", lx, ly, 18, kGold);
        ly += 24.0f;
        ui_.text("sniff_path", {lx, ly, lw - 90.0f, rh}, "File", pathInput_);
        if (ui_.button({lx + lw - 84.0f, ly, 84.0f, rh}, "Open")) loadFile(trim(pathInput_));
        ly += rh + 8.0f;

        // Discovered .flp / .fsc list
        const float listH = loaded_ ? 168.0f : (bodyH - (ly - bodyY) - 14.0f);
        const Rectangle fileBox{lx, ly, lw, listH};
        DrawRectangleRec(fileBox, Color{16, 12, 26, 220});
        DrawRectangleLinesEx(fileBox, 1.0f, Color{84, 70, 120, 180});
        if (files_.empty()) {
            gfx::text(FontId::Ui, "No .flp/.fsc files found in songs/ or mods/.", lx + 10, ly + 10, 16, kDim);
            gfx::text(FontId::Ui, "Drag & drop an FLP onto the window, or enter its path above.", lx + 10, ly + 32, 15, kDim);
        } else {
            const float rowH = touch ? 34.0f : 26.0f;
            ui_.beginScroll("sniff_files", fileBox, fileScroll_);
            for (int i = 0; i < int(files_.size()); ++i) {
                const float y = fileBox.y + 4.0f + float(i) * rowH - fileScroll_;
                if (y + rowH < fileBox.y || y > fileBox.y + fileBox.height) continue;
                const Rectangle r{fileBox.x + 4, y, fileBox.width - 14, rowH - 2};
                const bool sel = files_[size_t(i)] == loadedPath_;
                const bool hov = ui_.hover(r);
                if (sel || hov) DrawRectangleRec(r, sel ? Color{90, 66, 140, 230} : Color{50, 38, 76, 200});
                std::string label = files_[size_t(i)];
                if (label.rfind(a.base, 0) == 0) label = label.substr(a.base.size());
                gfx::text(FontId::Ui, label, r.x + 8, r.y + (rowH - 16.0f) * 0.5f, 16, sel ? kGold : kInk);
                if (hov && ui_.tap()) loadFile(files_[size_t(i)]);
            }
            ui_.endScroll(fileBox, fileScroll_, float(files_.size()) * rowH + 8.0f);
        }
        ly += listH + 10.0f;

        // Loaded project info & patterns
        if (loaded_) {
            gfx::text(FontId::Ui,
                      TextFormat("2. Patterns   |   BPM: %.2f   PPQN: %d   FPC ch: %d",
                                 proj_.bpm > 0 ? proj_.bpm : 120.0, proj_.ppqn, proj_.chartChannel),
                      lx, ly, 17, kGold);
            ly += 24.0f;
            const Rectangle patBox{lx, ly, lw, bodyY + bodyH - ly - 10.0f};
            DrawRectangleRec(patBox, Color{16, 12, 26, 220});
            DrawRectangleLinesEx(patBox, 1.0f, Color{84, 70, 120, 180});
            const float rowH = touch ? 36.0f : 28.0f;
            ui_.beginScroll("sniff_pats", patBox, patScroll_);
            for (int i = 0; i < int(proj_.patterns.size()); ++i) {
                const FlpPattern& pat = proj_.patterns[size_t(i)];
                const float y = patBox.y + 4.0f + float(i) * rowH - patScroll_;
                if (y + rowH < patBox.y || y > patBox.y + patBox.height) continue;
                const Rectangle r{patBox.x + 4, y, patBox.width - 14, rowH - 2};
                const bool sel = pat.number == pattern_;
                const bool hov = ui_.hover(r);
                if (sel || hov) DrawRectangleRec(r, sel ? Color{90, 66, 140, 230} : Color{50, 38, 76, 200});
                int lo = 127, hi = 0;
                for (const FlpNote& n : pat.notes) {
                    lo = std::min(lo, int(n.pitch));
                    hi = std::max(hi, int(n.pitch));
                }
                const std::string range = pat.notes.empty() ? std::string("empty")
                                                            : (midiNoteName(lo) + ".." + midiNoteName(hi) + " (" + std::to_string(lo) + "-" + std::to_string(hi) + ")");
                const std::string lbl = TextFormat("#%d %s  -  %zu notes  [%s]", pat.number,
                                                   pat.name.empty() ? "Pattern" : pat.name.c_str(), pat.notes.size(), range.c_str());
                gfx::text(FontId::Ui, lbl, r.x + 8, r.y + (rowH - 16.0f) * 0.5f, 15, sel ? kGold : kInk);
                if (hov && ui_.tap()) {
                    pattern_ = pat.number;
                    if (!pat.name.empty()) diffName_ = prettyName(pat.name);
                    updatePreview();
                }
            }
            ui_.endScroll(patBox, patScroll_, float(proj_.patterns.size()) * rowH + 8.0f);
        }

        // ---- Right column: Multikey MIDI ranges + Export
        const Rectangle rightBox{rightX, bodyY, rightW, bodyH};
        DrawRectangleRounded(rightBox, 0.02f, 6, kPanel);
        DrawRectangleRoundedLinesEx(rightBox, 0.02f, 6, 1.5f, Color{110, 92, 150, 150});
        const Rectangle rView{rightBox.x + 6, rightBox.y + 6, rightBox.width - 12, rightBox.height - 12};
        ui_.beginScroll("sniff_right", rView, rightScroll_);
        const float startY = rView.y + 4.0f - rightScroll_;
        float ry = startY;
        const float rx = rightBox.x + 14.0f, rw = rightBox.width - 34.0f;
        auto rrow = [&](float h = -1.0f) {
            if (h <= 0.0f) h = rh;
            Rectangle r{rx, ry, rw, h};
            ry += h + 7.0f;
            return r;
        };
        auto heading = [&](const std::string& s) {
            gfx::text(FontId::Ui, s, rx, ry, 18, kGold);
            ry += 24.0f;
        };

        heading("3. Multikey MIDI Note Ranges");
        // Quick key count presets
        {
            const float bw = (rw - 16.0f) / 5.0f;
            auto preset = [&](int idx, const char* label, int k) {
                if (ui_.button({rx + idx * (bw + 4.0f), ry, bw, rh}, label, oppKeys_ == k && plKeys_ == k)) {
                    oppKeys_ = plKeys_ = double(k);
                    plStart_ = 48;  // C3
                    oppStart_ = std::max(60, int(plStart_) + k + (k <= 4 ? 8 : 3));
                    if (k <= 9) oppStart_ = 60;  // C4 fits up to 9K cleanly above C3 (48..56)
                    updatePreview();
                }
            };
            preset(0, "4K", 4);
            preset(1, "6K", 6);
            preset(2, "7K", 7);
            preset(3, "9K", 9);
            if (ui_.button({rx + 4 * (bw + 4.0f), ry, bw, rh}, "Swap")) {
                std::swap(oppStart_, plStart_);
                std::swap(oppKeys_, plKeys_);
                updatePreview();
            }
            ry += rh + 8.0f;
        }

        // Opponent MIDI range
        bool changed = false;
        if (ui_.stepper(rrow(), "Opponent start MIDI", oppStart_, 1, 0, 127, "%.0f")) changed = true;
        if (ui_.stepper(rrow(), "Opponent keys", oppKeys_, 1, 1, kMaxKeys, "%.0f")) changed = true;
        gfx::text(FontId::Ui,
                  TextFormat("Opponent range: %s (%d) .. %s (%d)  [%dK]",
                             midiNoteName(int(oppStart_)).c_str(), int(oppStart_),
                             midiNoteName(std::min(127, int(oppStart_) + int(oppKeys_) - 1)).c_str(),
                             std::min(127, int(oppStart_) + int(oppKeys_) - 1), int(oppKeys_)),
                  rx, ry, 16, Color{255, 160, 180, 255});
        ry += 22.0f;

        // Player MIDI range
        if (ui_.stepper(rrow(), "Player start MIDI", plStart_, 1, 0, 127, "%.0f")) changed = true;
        if (ui_.stepper(rrow(), "Player keys", plKeys_, 1, 1, kMaxKeys, "%.0f")) changed = true;
        gfx::text(FontId::Ui,
                  TextFormat("Player range:   %s (%d) .. %s (%d)  [%dK]",
                             midiNoteName(int(plStart_)).c_str(), int(plStart_),
                             midiNoteName(std::min(127, int(plStart_) + int(plKeys_) - 1)).c_str(),
                             std::min(127, int(plStart_) + int(plKeys_) - 1), int(plKeys_)),
                  rx, ry, 16, Color{140, 235, 255, 255});
        ry += 24.0f;

        // Channel, BPM override, Markers
        if (ui_.choice(rrow(), "Channel", channelLabels_, channelIdx_)) changed = true;
        if (ui_.stepper(rrow(), "BPM override (0=auto)", bpmOverride_, 1, 0, 2000, "%.2f")) changed = true;
        if (ui_.check(rrow(), "SNIFF camera/alt markers (F3/F#3/A3/A#3)", useMarkers_)) changed = true;
        if (changed) updatePreview();

        // Live match preview box
        {
            const Rectangle pbox{rx, ry, rw, 54.0f};
            DrawRectangleRounded(pbox, 0.14f, 6, Color{18, 14, 30, 220});
            DrawRectangleRoundedLinesEx(pbox, 0.14f, 6, 1.0f, previewOk_ ? Color{110, 220, 150, 180} : Color{220, 110, 110, 180});
            if (!loaded_) {
                gfx::text(FontId::Ui, "Open a .flp or .fsc file to preview matched notes.", rx + 10, ry + 18, 16, kDim);
            } else if (previewOk_) {
                gfx::text(FontId::Ui,
                          TextFormat("Matched: %llu player, %llu opponent (%zu sections, %dK)",
                                     (unsigned long long)previewPl_, (unsigned long long)previewOpp_, previewSecs_,
                                     std::max(int(oppKeys_), int(plKeys_))),
                          rx + 10, ry + 8, 16, Color{150, 240, 175, 255});
                gfx::text(FontId::Ui,
                          TextFormat("BPM %.2f   |   Scroll speed %.2f", previewBpm_, previewSpeed_),
                          rx + 10, ry + 30, 15, kDim);
            } else {
                gfx::text(FontId::Ui, "Preview: " + previewErr_, rx + 10, ry + 18, 16, Color{255, 140, 140, 255});
            }
            ry += 62.0f;
        }

        heading("4. Convert & Export");
        ui_.text("sniff_song", rrow(), "Song folder", songFolder_);
        ui_.text("sniff_diff", rrow(), "Difficulty", diffName_);
        ui_.choice(rrow(), "Save format", {"Psych JSON", "Binary .ghc", "Both"}, saveFmt_);
        if (saveFmt_ != 1) ui_.check(rrow(), "Minify Psych JSON (remove formatting)", minifyJson_);

        const float bw = (rw - 8.0f) * 0.5f;
        if (ui_.button({rx, ry, bw, rh + 6}, "Convert & Save", true)) convertAndSave();
        if (ui_.button({rx + bw + 8.0f, ry, bw, rh + 6}, "Open in Chart Editor")) {
            if (!lastSavedSongId_.empty()) {
                ui_.endScroll(rView, rightScroll_, ry - startY + rh + 20.0f);
                a.change(makeChartEditor(lastSavedSongId_, lastSavedDiff_));
                gfx::endCanvas();
                return;
            }
            status_.set("Convert & Save first to open in the Chart Editor", true);
        }
        ry += rh + 14.0f;
        gfx::text(FontId::Ui, "Output dir: " + outputDir(), rx, ry, 14, kDim);
        ry += 22.0f;

        ui_.endScroll(rView, rightScroll_, ry - startY + 12.0f);
        ui_.overlays();
        status_.draw();
        hints("Drag & drop .flp/.fsc anywhere   Adjust Opponent & Player MIDI ranges for multikey   ESC back");
        gfx::endCanvas();
    }

private:
    ModChoice mod_;
    Ui ui_;
    Status status_;
    std::vector<std::string> files_;
    std::string pathInput_, loadedPath_;
    FlpProject proj_;
    bool loaded_ = false;
    int pattern_ = -1;
    double oppStart_ = 60.0, oppKeys_ = 4.0;
    double plStart_ = 48.0, plKeys_ = 4.0;
    std::vector<std::string> channelLabels_{"Auto (FPC)", "All channels"};
    std::vector<int> channelValues_{-1, -2};
    int channelIdx_ = 0;
    double bpmOverride_ = 0.0;
    bool useMarkers_ = true;
    std::string songFolder_ = "sniff-song", diffName_ = "Normal";
    int saveFmt_ = 0;
    bool minifyJson_ = false;
    float fileScroll_ = 0.0f, patScroll_ = 0.0f, rightScroll_ = 0.0f;
    // Preview cache
    bool previewOk_ = false;
    std::string previewErr_;
    uint64_t previewPl_ = 0, previewOpp_ = 0;
    size_t previewSecs_ = 0;
    double previewBpm_ = 120.0, previewSpeed_ = 2.0;
    std::string lastSavedSongId_, lastSavedDiff_;

    void scanFiles() {
        files_.clear();
        const App& a = app();
        std::set<std::string> seen;
        auto walk = [&](const fs::path& root) {
            std::error_code ec;
            if (!fs::exists(root, ec)) return;
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
                if (!it->is_regular_file(ec)) continue;
                const std::string p = pathStr(it->path());
                if ((endsWithI(p, ".flp") || endsWithI(p, ".fsc")) && seen.insert(p).second) files_.push_back(p);
            }
        };
        walk(toPath(a.base + "songs"));
        walk(toPath(a.base + "mods"));
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(toPath(a.base), ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string p = pathStr(e.path());
            if ((endsWithI(p, ".flp") || endsWithI(p, ".fsc")) && seen.insert(p).second) files_.push_back(p);
        }
        std::sort(files_.begin(), files_.end());
    }

    void loadFile(const std::string& path) {
        if (path.empty()) return;
        FlpProject p;
        std::string err;
        if (!readFlpFile(path, p, err)) {
            status_.set("SNIFF: " + err, true);
            return;
        }
        proj_ = std::move(p);
        loaded_ = true;
        loadedPath_ = path;
        pathInput_ = path;
        std::vector<int> cp = flpChartPatterns(proj_, true);
        if (cp.empty()) cp = flpChartPatterns(proj_, false);
        if (!cp.empty()) pattern_ = cp.front();
        else if (!proj_.patterns.empty()) pattern_ = proj_.patterns.front().number;
        else pattern_ = -1;

        // Gather channels present in project
        std::set<int> chs;
        for (const FlpPattern& pat : proj_.patterns)
            for (const FlpNote& n : pat.notes) chs.insert(int(n.channel));
        channelLabels_ = {"Auto (FPC ch " + std::to_string(proj_.chartChannel) + ")", "All channels"};
        channelValues_ = {-1, -2};
        for (int c : chs) {
            channelLabels_.push_back("Channel " + std::to_string(c));
            channelValues_.push_back(c);
        }
        channelIdx_ = 0;

        const std::string stem = pathStr(toPath(path).stem());
        songFolder_ = songPathName(proj_.title.empty() ? stem : proj_.title);
        if (songFolder_.empty()) songFolder_ = "sniff-song";
        diffName_ = pattern_ >= 0 ? flpPatternLabel(proj_, pattern_) : std::string("Normal");
        if (diffName_.rfind("Pattern ", 0) == 0) diffName_ = "Normal";
        updatePreview();
        status_.set("Loaded " + pathStr(toPath(path).filename()) + " (" + std::to_string(proj_.patterns.size()) + " patterns)");
    }

    FlpConvertOptions currentOpt() const {
        FlpConvertOptions o;
        o.oppStartPitch = int(std::lround(oppStart_));
        o.oppKeys = int(std::lround(oppKeys_));
        o.playerStartPitch = int(std::lround(plStart_));
        o.playerKeys = int(std::lround(plKeys_));
        o.channel = (channelIdx_ >= 0 && channelIdx_ < int(channelValues_.size())) ? channelValues_[size_t(channelIdx_)] : -1;
        o.bpmOverride = bpmOverride_;
        o.useMarkers = useMarkers_;
        return o;
    }

    void updatePreview() {
        previewOk_ = false;
        previewErr_.clear();
        if (!loaded_) return;
        Chart c;
        std::string err;
        if (!flpToChart(proj_, pattern_, currentOpt(), c, err)) {
            previewErr_ = err;
            return;
        }
        previewOk_ = true;
        previewPl_ = c.playerNotes;
        previewOpp_ = c.opponentNotes;
        previewSecs_ = c.sections.size();
        previewBpm_ = c.bpmAt(0);
        previewSpeed_ = c.speed;
    }

    std::string outputDir() const {
        const std::string key = songPathName(songFolder_.empty() ? "sniff-song" : songFolder_);
        if (mod_.name.empty()) return app().base + "songs/" + key + "/";
        return mod_.roots[0] + "data/" + key + "/";
    }

    std::string chartToPsychJson(const Chart& c, StreamedNotesBuilder* bin, bool compact) const {
        const char* col = compact ? ":" : ": ";
        const char* com = compact ? "," : ", ";
        const char* nl2 = compact ? "" : "\n  ";
        const char* nl4 = compact ? "" : "\n    ";
        const int K = c.keys;
        std::string o;
        o.reserve(c.notes.size() * 24 + 2048);
        const std::string title = c.title.empty() ? prettyName(songFolder_) : c.title;
        o += std::string("{\"song\"") + col + "{" + nl2 + "\"song\"" + col + jsonStr(title) + "," + nl2 + "\"bpm\"" + col +
             jnum(c.bpmAt(0)) + "," + nl2 + "\"speed\"" + col + jnum(c.speed) + "," + nl2 + "\"needsVoices\"" + col + "false," +
             nl2 + "\"player1\"" + col + "\"bf\"," + nl2 + "\"player2\"" + col + "\"dad\"," + nl2 + "\"gfVersion\"" + col + "\"gf\"," +
             nl2 + "\"stage\"" + col + "\"stage\"";
        if (K != 4 || c.sideKeys[0] || c.sideKeys[1]) {
            o += std::string(",") + nl2 + "\"keyCount\"" + col + std::to_string(c.keysOf(0)) + "," + nl2 + "\"playerKeyCount\"" + col +
                 std::to_string(c.keysOf(1)) + "," + nl2 + "\"mania\"" + col + std::to_string(K - 1);
        }
        o += std::string(",") + nl2 + "\"events\"" + col + "[]," + nl2 + "\"notes\"" + col + "[";
        size_t ni = 0;
        const size_t nSec = std::max<size_t>(1, c.sections.size());
        for (size_t s = 0; s < nSec; ++s) {
            const SectionInfo sec = s < c.sections.size() ? c.sections[s] : SectionInfo{0, true, false, false, 4, c.bpmAt(0)};
            const double b = (s + 1 < c.sections.size()) ? c.sections[s + 1].timeMs : 1e18;
            o += std::string(s ? "," : "") + nl4 + "{\"sectionBeats\"" + col + jnum(sec.beats) + com + "\"lengthInSteps\"" + col +
                 std::to_string(int(std::lround(sec.beats * 4))) + com + "\"mustHitSection\"" + col +
                 (sec.mustHit ? "true" : "false") + com + "\"changeBPM\"" + col + "false" + com + "\"bpm\"" + col + jnum(sec.bpm) +
                 com + "\"altAnim\"" + col + (sec.alt ? "true" : "false") + com + "\"gfSection\"" + col + "false" + com +
                 "\"sectionNotes\"" + col + (bin ? bin->beginArray() : std::string("["));
            bool first = true;
            while (ni < c.notes.size() && (c.notes[ni].time < b - 0.5 || s + 1 == nSec)) {
                const Note& n = c.notes[ni++];
                const bool player = n.side == 1;
                const int data = (player == sec.mustHit) ? int(n.lane) : int(n.lane) + K;
                const bool alt = (n.flags & kFlagAlt) != 0;
                if (bin) {
                    const std::string t = alt ? std::string("Alt Animation") : std::string();
                    for (uint16_t k = 0; k < std::max<uint16_t>(1, n.stack); ++k) bin->add(n.time, data, n.length, t);
                    continue;
                }
                std::string one = "[" + jnum(n.time) + com + std::to_string(data) + com + jnum(n.length);
                if (alt) one += std::string(com) + "\"Alt Animation\"";
                one += "]";
                for (uint16_t k = 0; k < std::max<uint16_t>(1, n.stack); ++k) {
                    o += std::string(first ? "" : com) + one;
                    first = false;
                }
            }
            o += bin ? "}" : "]}";
        }
        o += compact ? "]}}" : "\n  ]\n}}\n";
        if (bin) bin->finish();
        return o;
    }

    void convertAndSave() {
        if (!loaded_) {
            status_.set("Load an .flp or .fsc file first", true);
            return;
        }
        Chart c;
        std::string err;
        if (!flpToChart(proj_, pattern_, currentOpt(), c, err)) {
            status_.set("SNIFF: " + err, true);
            return;
        }
        const std::string key = songPathName(songFolder_.empty() ? "sniff-song" : songFolder_);
        const std::string dLower = toLower(trim(diffName_));
        const std::string stem = key + (dLower.empty() || dLower == "normal" ? std::string() : "-" + songPathName(dLower));
        const std::string dir = outputDir();
        std::error_code ec;
        fs::create_directories(toPath(dir), ec);
        const std::string jsonPath = dir + stem + ".json";
        const std::string ghcPath = dir + stem + kBinaryChartExt;
        const bool wantJson = saveFmt_ != 1, wantGhc = saveFmt_ != 0;
        if (wantJson) {
            if (!saveText(jsonPath, chartToPsychJson(c, nullptr, minifyJson_), err)) {
                status_.set(err, true);
                return;
            }
        }
        if (wantGhc) {
            StreamedNotes sn;
            StreamedNotesBuilder bin(sn);
            const std::string skel = chartToPsychJson(c, &bin, true);
            if (!writeBinaryChart(ghcPath, skel, sn, 3, err)) {
                status_.set("GHC: " + err, true);
                return;
            }
        }
        // Rescan library so the new chart shows up in Freeplay & Chart Editor.
        App& a = app();
        a.library.scan(a.base);
        lastSavedSongId_.clear();
        lastSavedDiff_ = diffName_.empty() ? "Normal" : diffName_;
        for (const SongEntry& s : a.library.songs()) {
            for (size_t i = 0; i < s.chartPaths.size(); ++i) {
                const std::string& cp = s.chartPaths[i];
                if (fs::equivalent(toPath(cp), toPath(wantJson ? jsonPath : ghcPath), ec)) {
                    lastSavedSongId_ = s.id;
                    if (i < s.diffs.size()) lastSavedDiff_ = s.diffs[i];
                    break;
                }
            }
            if (!lastSavedSongId_.empty()) break;
        }
        status_.set(TextFormat("Converted %llu notes (%dK) -> %s", (unsigned long long)(c.playerNotes + c.opponentNotes),
                               c.keys, (wantJson ? jsonPath : ghcPath).c_str()));
    }
};

// ====================================================== modchart editor ==
// Visual timeline editor for timed Psych-compatible Lua modcharts. Generates
// and reloads `modchart.lua` inside the selected song's data folder.

class ModchartEditorState : public State {
public:
    bool keyReserved(int) const override { return !ui_.focus.empty(); }
    TouchLayout touchLayout() const override {
        TouchLayout t;
        t.pad = false;
        return t;
    }

    void enter() override {
        mod_ = currentMod();
        refreshSongs();
        if (!songs_.empty()) selectSong(0);
    }

    void update(double dt) override {
        App& a = app();
        Input& in = a.input;
        feedUi(ui_);
        if (playing_) {
            curBeat_ += dt * (bpm_ / 60.0);
            if (curBeat_ > maxBeat_) curBeat_ = 0.0;
        }
        if (ui_.focus.empty() && ui_.dropId.empty()) {
            if (in.pressed(KEY_SPACE)) playing_ = !playing_;
            if (in.pressed(KEY_R)) {
                playing_ = false;
                curBeat_ = 0.0;
            }
            if (in.back()) {
                sfxPlay(Sfx::Cancel);
                a.change(makeEditorMenu());
                return;
            }
        }
    }

    void draw() override {
        App& a = app();
        const bool touch = a.touchMode;
        if (ui_.tap() && ui_.askId.empty()) ui_.focus.clear();
        gfx::beginCanvas();
        ClearBackground(Color{16, 12, 24, 255});
        DrawRectangleRec({0, 0, gfx::W, gfx::H}, Color{16, 12, 24, 255});

        // Top bar
        const float topH = touch ? 50.0f : 44.0f;
        DrawRectangleRec({0, 0, gfx::W, topH}, Color{8, 5, 16, 245});
        DrawRectangleRec({0, topH - 1, gfx::W, 1}, Color{110, 92, 150, 150});
        if (ui_.button({10, 6, 98, topH - 12}, "< BACK")) {
            sfxPlay(Sfx::Cancel);
            a.change(makeEditorMenu());
            gfx::endCanvas();
            return;
        }
        gfx::text(FontId::Title, "MODCHART EDITOR", 118, 9, touch ? 24.0f : 26.0f, kGold);
        if (!songLabels_.empty()) {
            if (ui_.dropdown("mc_song", {370, 7, 300, topH - 14}, "Song", songLabels_, songIdx_, 52)) {
                selectSong(songIdx_);
            }
        }
        if (ui_.button({682, 7, 96, topH - 14}, playing_ ? "PAUSE" : "PLAY", playing_)) playing_ = !playing_;
        if (ui_.button({784, 7, 82, topH - 14}, "RESET")) {
            playing_ = false;
            curBeat_ = 0.0;
        }
        gfx::text(FontId::Ui, TextFormat("Beat %.2f", curBeat_), 880, 13, 18, kGold);
        if (ui_.button({gfx::W - 188, 7, 176, topH - 14}, "Save modchart.lua", true)) saveModchart();

        const float bodyY = topH + 8.0f;
        const float panelXPos = 816.0f;
        const float stageW = panelXPos - 22.0f;
        const float stageH = gfx::H - bodyY - (touch ? 12.0f : 48.0f);

        // ---- Left: Live animated strumline preview + beat scrubber
        drawPreviewStage({12.0f, bodyY, stageW, stageH});

        // ---- Right: Events list & inspector
        const Rectangle pRect{panelXPos, bodyY, gfx::W - panelXPos - 12.0f, stageH};
        DrawRectangleRounded(pRect, 0.02f, 6, kPanel);
        DrawRectangleRoundedLinesEx(pRect, 0.02f, 6, 1.5f, Color{110, 92, 150, 160});
        const Rectangle view{pRect.x + 6, pRect.y + 6, pRect.width - 12, pRect.height - 12};
        ui_.beginScroll("mc_panel", view, panelScroll_);
        const float startY = view.y + 4.0f - panelScroll_;
        float y = startY;
        const float x = pRect.x + 12.0f, w = pRect.width - 30.0f;
        const float rh = touch ? 36.0f : 30.0f;
        auto row = [&](float h = -1.0f) {
            if (h <= 0.0f) h = rh;
            Rectangle r{x, y, w, h};
            y += h + 7.0f;
            return r;
        };

        gfx::text(FontId::Ui, "Preview Settings", x, y, 17, kGold);
        y += 22.0f;
        ui_.stepper(row(), "Preview BPM", bpm_, 1, 30, 600, "%.1f");
        double pk = previewKeys_;
        if (ui_.stepper(row(), "Preview Keys", pk, 1, 1, 9, "%.0f")) previewKeys_ = int(pk);

        y += 4.0f;
        gfx::text(FontId::Ui, TextFormat("Modchart Events (%zu)", events_.size()), x, y, 17, kGold);
        y += 24.0f;
        {
            const float bw = (w - 12.0f) / 3.0f;
            if (ui_.button({x, y, bw, rh}, "+ Add")) addEvent();
            if (ui_.button({x + bw + 6, y, bw, rh}, "Duplicate") && sel_ >= 0 && sel_ < int(events_.size())) {
                ModEvent copy = events_[size_t(sel_)];
                copy.beat += 1.0;
                events_.push_back(copy);
                sortEvents(copy);
            }
            if (ui_.button({x + 2 * (bw + 6), y, bw, rh}, "Delete") && sel_ >= 0 && sel_ < int(events_.size())) {
                events_.erase(events_.begin() + sel_);
                sel_ = std::min(sel_, int(events_.size()) - 1);
            }
            y += rh + 8.0f;
        }

        // Compact event list
        const float listH = 165.0f;
        const Rectangle ebox{x, y, w, listH};
        DrawRectangleRec(ebox, Color{14, 10, 24, 230});
        DrawRectangleLinesEx(ebox, 1.0f, Color{84, 70, 120, 170});
        if (events_.empty()) {
            gfx::text(FontId::Ui, "No events yet. Tap '+ Add' to create one.", ebox.x + 8, ebox.y + 10, 15, kDim);
        } else {
            const float itemH = touch ? 32.0f : 25.0f;
            const int vis = std::max(1, int((listH - 6) / itemH));
            if (CheckCollisionPointRec(ui_.mouse, ebox) && ui_.wheel != 0)
                listScroll_ = clampv(listScroll_ - ui_.wheel * 2.0f, 0.0f, float(std::max(0, int(events_.size()) - vis)));
            const int first = std::clamp(int(listScroll_), 0, std::max(0, int(events_.size()) - 1));
            for (int k = 0; k < vis && first + k < int(events_.size()); ++k) {
                const int i = first + k;
                const ModEvent& ev = events_[size_t(i)];
                const Rectangle r{ebox.x + 3, ebox.y + 3 + float(k) * itemH, ebox.width - 6, itemH - 2};
                const bool hov = ui_.hover(r);
                if (i == sel_ || hov) DrawRectangleRec(r, i == sel_ ? Color{90, 66, 140, 240} : Color{50, 38, 76, 200});
                const std::string line = TextFormat("B%.2f  %s (%s) -> %.2g", ev.beat, kPropNames[ev.prop], kTargetNames[ev.target], ev.value);
                gfx::text(FontId::Ui, line, r.x + 6, r.y + (itemH - 15.0f) * 0.5f, 15, i == sel_ ? kGold : kInk);
                if (hov && ui_.tap()) {
                    sel_ = i;
                    curBeat_ = ev.beat;
                }
            }
        }
        y += listH + 10.0f;

        // Selected event inspector
        if (sel_ >= 0 && sel_ < int(events_.size())) {
            ModEvent& ev = events_[size_t(sel_)];
            gfx::text(FontId::Ui, TextFormat("Edit Event #%d", sel_ + 1), x, y, 17, kGold);
            y += 24.0f;
            if (ui_.stepper(row(), "Beat", ev.beat, 0.25, 0.0, 1024.0, "%.2f")) {
                ModEvent snap = ev;
                sortEvents(snap);
            }
            ui_.stepper(row(), "Duration (beats)", ev.dur, 0.25, 0.0, 64.0, "%.2f");
            ui_.dropdown("mc_prop", row(), "Property", propList_, ev.prop, 84);
            ui_.dropdown("mc_tgt", row(), "Target", targetList_, ev.target, 84);
            const double step = (ev.prop == kPropAlpha || ev.prop == kPropScroll || ev.prop == kPropCamZoom) ? 0.1 : 10.0;
            const double lo = (ev.prop == kPropAlpha) ? 0.0 : (ev.prop == kPropScroll || ev.prop == kPropCamZoom) ? 0.1 : -1280.0;
            const double hi = (ev.prop == kPropAlpha) ? 1.0 : (ev.prop == kPropScroll || ev.prop == kPropCamZoom) ? 10.0 : 1280.0;
            ui_.stepper(row(), "Value", ev.value, step, lo, hi, "%.2f");
            ui_.dropdown("mc_ease", row(), "Ease", easeList_, ev.ease, 84);
        }

        ui_.endScroll(view, panelScroll_, y - startY + 24.0f);
        ui_.overlays();
        status_.draw();
        hints("SPACE play/pause   R reset   Drag timeline to scrub   Saves modchart.lua into the song folder   ESC back");
        gfx::endCanvas();
    }

private:
    enum Prop {
        kPropStrumX = 0,
        kPropStrumY,
        kPropStrumAngle,
        kPropAlpha,
        kPropScroll,
        kPropCamZoom,
        kPropCamAngle,
        kPropDrunk,
        kPropTipsy,
        kPropBounce,
        kPropCount
    };
    static constexpr const char* kPropNames[kPropCount] = {"Strum X",   "Strum Y",      "Strum Angle", "Strum Alpha", "Scroll Speed",
                                                           "Cam Zoom",  "Cam Angle",    "Drunk (X)",   "Tipsy (Y)",   "Bounce"};
    static constexpr const char* kTargetNames[] = {"Both", "Player", "Opponent", "Lane 0", "Lane 1", "Lane 2",
                                                   "Lane 3", "Lane 4", "Lane 5", "Lane 6", "Lane 7", "Lane 8"};
    static constexpr const char* kEaseNames[] = {"linear", "quadOut", "quadInOut", "cubeOut", "sineInOut", "elasticOut", "backOut", "expoOut"};

    struct ModEvent {
        double beat = 0.0;
        double dur = 1.0;
        int prop = kPropStrumY;
        int target = 0;  // 0 Both, 1 Player, 2 Opponent, 3+ specific lane (target - 3)
        double value = 40.0;
        int ease = 1;
    };

    ModChoice mod_;
    Ui ui_;
    Status status_;
    std::vector<const SongEntry*> songs_;
    std::vector<std::string> songLabels_;
    int songIdx_ = 0;
    std::vector<ModEvent> events_;
    int sel_ = -1;
    bool playing_ = false;
    double curBeat_ = 0.0, maxBeat_ = 64.0, bpm_ = 120.0;
    int previewKeys_ = 4;
    float panelScroll_ = 0.0f, listScroll_ = 0.0f;
    bool scrubbing_ = false;
    std::vector<std::string> propList_{std::begin(kPropNames), std::end(kPropNames)};
    std::vector<std::string> targetList_{std::begin(kTargetNames), std::end(kTargetNames)};
    std::vector<std::string> easeList_{std::begin(kEaseNames), std::end(kEaseNames)};

    void refreshSongs() {
        songs_.clear();
        songLabels_.clear();
        for (const SongEntry& e : app().library.songs()) {
            if (e.dataDir.empty()) continue;
            songs_.push_back(&e);
            songLabels_.push_back(e.title + (e.pack.empty() ? "" : " [" + e.pack + "]"));
        }
        songIdx_ = std::clamp(songIdx_, 0, std::max(0, int(songs_.size()) - 1));
    }

    std::string modchartPath() const {
        if (songIdx_ < 0 || songIdx_ >= int(songs_.size())) return app().base + "mods/modchart.lua";
        const std::string& d = songs_[size_t(songIdx_)]->dataDir;
        return d + (!d.empty() && d.back() == '/' ? "" : "/") + "modchart.lua";
    }

    void selectSong(int idx) {
        songIdx_ = std::clamp(idx, 0, std::max(0, int(songs_.size()) - 1));
        events_.clear();
        sel_ = -1;
        curBeat_ = 0.0;
        playing_ = false;
        bpm_ = 120.0;
        if (songIdx_ >= 0 && songIdx_ < int(songs_.size())) {
            const SongEntry& se = *songs_[size_t(songIdx_)];
            if (!se.chartPaths.empty() && !endsWithI(se.chartPaths[0], ".ghc")) {
                std::string chartTxt;
                if (readTextFile(se.chartPaths[0], chartTxt)) {
                    const size_t bp = chartTxt.find("\"bpm\"");
                    if (bp != std::string::npos) {
                        const size_t col = chartTxt.find(':', bp);
                        if (col != std::string::npos) {
                            const double v = std::atof(chartTxt.c_str() + col + 1);
                            if (v >= 20.0 && v <= 999.0) bpm_ = v;
                        }
                    }
                }
            }
        }
        loadModchart();
    }

    void addEvent() {
        ModEvent ev;
        ev.beat = std::round(curBeat_ * 4.0) * 0.25;
        ev.dur = 1.0;
        ev.prop = kPropDrunk;
        ev.target = 0;
        ev.value = 30.0;
        ev.ease = 1;
        events_.push_back(ev);
        sortEvents(ev);
    }

    void sortEvents(const ModEvent& track) {
        std::stable_sort(events_.begin(), events_.end(), [](const ModEvent& a, const ModEvent& b) { return a.beat < b.beat; });
        sel_ = 0;
        maxBeat_ = 32.0;
        for (int i = 0; i < int(events_.size()); ++i) {
            maxBeat_ = std::max(maxBeat_, events_[size_t(i)].beat + events_[size_t(i)].dur + 8.0);
            if (std::fabs(events_[size_t(i)].beat - track.beat) < 1e-6 && events_[size_t(i)].prop == track.prop &&
                events_[size_t(i)].target == track.target)
                sel_ = i;
        }
    }

    void loadModchart() {
        std::string txt;
        if (!readTextFile(modchartPath(), txt)) return;
        size_t pos = 0;
        while (pos < txt.size()) {
            size_t end = txt.find('\n', pos);
            if (end == std::string::npos) end = txt.size();
            const std::string line = trim(txt.substr(pos, end - pos));
            pos = end + 1;
            if (line.rfind("-- EVT ", 0) == 0) {
                ModEvent ev;
                if (std::sscanf(line.c_str() + 7, "%lf %lf %d %d %lf %d", &ev.beat, &ev.dur, &ev.prop, &ev.target, &ev.value, &ev.ease) == 6) {
                    ev.prop = std::clamp(ev.prop, 0, int(kPropCount) - 1);
                    ev.target = std::clamp(ev.target, 0, int(targetList_.size()) - 1);
                    ev.ease = std::clamp(ev.ease, 0, int(easeList_.size()) - 1);
                    events_.push_back(ev);
                }
            }
        }
        if (!events_.empty()) {
            sortEvents(events_[0]);
            status_.set("Loaded " + std::to_string(events_.size()) + " modchart events");
        }
    }

    void saveModchart() {
        std::string lua;
        lua += "-- ASTRALIS_MODCHART_V1\n";
        lua += "-- Generated by Astralis Modchart Editor (Psych Engine Lua compatible)\n";
        for (const ModEvent& ev : events_) {
            lua += TextFormat("-- EVT %.4f %.4f %d %d %.4f %d\n", ev.beat, ev.dur, ev.prop, ev.target, ev.value, ev.ease);
        }
        lua += "\nlocal mcEvents = {\n";
        for (size_t i = 0; i < events_.size(); ++i) {
            const ModEvent& ev = events_[i];
            lua += TextFormat("    {beat = %.4f, dur = %.4f, prop = %d, target = %d, value = %.4f, ease = '%s', fired = false},\n",
                              ev.beat, ev.dur, ev.prop, ev.target, ev.value, kEaseNames[ev.ease]);
        }
        lua += "}\n\n";
        lua += R"LUA(local baseX, baseY = {}, {}
local drunkAmp, tipsyAmp, bounceAmp = 0, 0, 0
local baseZoom = 1

function onCreatePost()
    baseZoom = getProperty('defaultCamZoom') or 1
    local n = getProperty('strumLineNotes.length') or 8
    for i = 0, n - 1 do
        baseX[i] = getPropertyFromGroup('strumLineNotes', i, 'x')
        baseY[i] = getPropertyFromGroup('strumLineNotes', i, 'y')
    end
end

local function lanesFor(target)
    local total = getProperty('strumLineNotes.length') or 8
    local half = math.floor(total / 2)
    local out = {}
    if target == 0 then
        for i = 0, total - 1 do out[#out + 1] = i end
    elseif target == 1 then
        for i = half, total - 1 do out[#out + 1] = i end
    elseif target == 2 then
        for i = 0, half - 1 do out[#out + 1] = i end
    else
        local lane = target - 3
        if lane < half then out[#out + 1] = lane end
        if half + lane < total then out[#out + 1] = half + lane end
    end
    return out
end

local function fireEvent(idx, ev)
    local sec = math.max(0.001, (ev.dur or 0) * (crochet / 1000))
    local lanes = lanesFor(ev.target)
    if ev.prop == 0 then
        for _, l in ipairs(lanes) do
            noteTweenX('mcX_' .. idx .. '_' .. l, l, (baseX[l] or 0) + ev.value, sec, ev.ease)
        end
    elseif ev.prop == 1 then
        for _, l in ipairs(lanes) do
            noteTweenY('mcY_' .. idx .. '_' .. l, l, (baseY[l] or 50) + ev.value, sec, ev.ease)
        end
    elseif ev.prop == 2 then
        for _, l in ipairs(lanes) do
            noteTweenAngle('mcA_' .. idx .. '_' .. l, l, ev.value, sec, ev.ease)
        end
    elseif ev.prop == 3 then
        for _, l in ipairs(lanes) do
            noteTweenAlpha('mcAl_' .. idx .. '_' .. l, l, math.max(0, math.min(1, ev.value)), sec, ev.ease)
        end
    elseif ev.prop == 4 then
        setProperty('songSpeed', math.max(0.1, ev.value))
    elseif ev.prop == 5 then
        doTweenZoom('mcZoom_' .. idx, 'camGame', math.max(0.1, ev.value), sec, ev.ease)
        setProperty('defaultCamZoom', math.max(0.1, ev.value))
    elseif ev.prop == 6 then
        doTweenAngle('mcCamA_' .. idx, 'camHUD', ev.value, sec, ev.ease)
    elseif ev.prop == 7 then
        drunkAmp = ev.value
    elseif ev.prop == 8 then
        tipsyAmp = ev.value
    elseif ev.prop == 9 then
        bounceAmp = ev.value
    end
end

function onUpdate(elapsed)
    local beat = curDecBeat or 0
    for i, ev in ipairs(mcEvents) do
        if not ev.fired and beat >= ev.beat then
            ev.fired = true
            fireEvent(i, ev)
        end
    end
    if drunkAmp ~= 0 or tipsyAmp ~= 0 or bounceAmp ~= 0 then
        local total = getProperty('strumLineNotes.length') or 8
        for i = 0, total - 1 do
            if baseX[i] and baseY[i] then
                local dx = drunkAmp * math.sin(beat * 1.5 + i * 0.65)
                local dy = tipsyAmp * math.cos(beat * 2.0 + i * 0.8) + bounceAmp * math.abs(math.sin(beat * math.pi))
                if drunkAmp ~= 0 then setPropertyFromGroup('strumLineNotes', i, 'x', baseX[i] + dx) end
                if tipsyAmp ~= 0 or bounceAmp ~= 0 then setPropertyFromGroup('strumLineNotes', i, 'y', baseY[i] + dy) end
            end
        end
    end
end
)LUA";
        std::string err;
        const std::string p = modchartPath();
        if (!saveText(p, lua, err)) {
            status_.set(err, true);
            return;
        }
        status_.set("Saved " + p + " (" + std::to_string(events_.size()) + " events)");
    }

    struct EvalState {
        float ox[2][9]{}, oy[2][9]{}, ang[2][9]{}, alpha[2][9]{};
        float scroll = 2.0f, camZoom = 1.0f, camAngle = 0.0f;
        float drunk = 0.0f, tipsy = 0.0f, bounce = 0.0f;
    };

    EvalState evaluateAt(double beat) const {
        EvalState s;
        for (int side = 0; side < 2; ++side)
            for (int l = 0; l < 9; ++l) s.alpha[side][l] = 1.0f;
        for (const ModEvent& ev : events_) {
            if (beat < ev.beat) continue;
            const double t = ev.dur > 1e-5 ? std::clamp((beat - ev.beat) / ev.dur, 0.0, 1.0) : 1.0;
            const float k = float(psych::ease(kEaseNames[ev.ease], t));
            auto apply = [&](auto fn) {
                for (int side = 0; side < 2; ++side)
                    for (int l = 0; l < previewKeys_; ++l) {
                        if (ev.target == 1 && side != 1) continue;
                        if (ev.target == 2 && side != 0) continue;
                        if (ev.target >= 3 && l != ev.target - 3) continue;
                        fn(side, l);
                    }
            };
            switch (ev.prop) {
                case kPropStrumX: apply([&](int sd, int l) { s.ox[sd][l] += (float(ev.value) - s.ox[sd][l]) * k; }); break;
                case kPropStrumY: apply([&](int sd, int l) { s.oy[sd][l] += (float(ev.value) - s.oy[sd][l]) * k; }); break;
                case kPropStrumAngle: apply([&](int sd, int l) { s.ang[sd][l] += (float(ev.value) - s.ang[sd][l]) * k; }); break;
                case kPropAlpha: apply([&](int sd, int l) { s.alpha[sd][l] += (float(std::clamp(ev.value, 0.0, 1.0)) - s.alpha[sd][l]) * k; }); break;
                case kPropScroll: s.scroll = std::max(0.1f, float(ev.value)); break;
                case kPropCamZoom: s.camZoom += (std::max(0.2f, float(ev.value)) - s.camZoom) * k; break;
                case kPropCamAngle: s.camAngle += (float(ev.value) - s.camAngle) * k; break;
                case kPropDrunk: s.drunk = float(ev.value); break;
                case kPropTipsy: s.tipsy = float(ev.value); break;
                case kPropBounce: s.bounce = float(ev.value); break;
                default: break;
            }
        }
        for (int side = 0; side < 2; ++side)
            for (int l = 0; l < previewKeys_; ++l) {
                const int idx = side * previewKeys_ + l;
                s.ox[side][l] += s.drunk * std::sin(float(beat) * 1.5f + float(idx) * 0.65f);
                s.oy[side][l] += s.tipsy * std::cos(float(beat) * 2.0f + float(idx) * 0.8f) +
                                 s.bounce * std::fabs(std::sin(float(beat) * 3.14159265f));
            }
        return s;
    }

    void drawPreviewStage(Rectangle area) {
        DrawRectangleRounded(area, 0.02f, 6, Color{10, 8, 18, 240});
        DrawRectangleRoundedLinesEx(area, 0.02f, 6, 1.5f, Color{92, 78, 134, 170});
        const float barH = 56.0f;
        const Rectangle view{area.x + 8, area.y + 8, area.width - 16, area.height - barH - 16};
        DrawRectangleRec(view, Color{18, 14, 30, 255});

        const EvalState st = evaluateAt(curBeat_);
        const Vector2 center{view.x + view.width * 0.5f, view.y + view.height * 0.5f};
        const float scale = (view.width / 1280.0f) * st.camZoom;
        const float rad = st.camAngle * 0.01745329f;
        const float cs = std::cos(rad), sn = std::sin(rad);
        auto xform = [&](float gx, float gy) -> Vector2 {
            const float dx = (gx - 640.0f) * scale;
            const float dy = (gy - 360.0f) * scale;
            return {center.x + dx * cs - dy * sn, center.y + dx * sn + dy * cs};
        };

        // Subtle beat pulse backdrop grid
        const float pulse = float(1.0 - std::fmod(curBeat_, 1.0));
        DrawRectangleLinesEx(view, 1.5f, fade(kGold, 0.25f + 0.35f * pulse));
        const float lw = std::min(110.0f, 460.0f / float(previewKeys_));
        const float noteSz = lw * 0.86f * scale;
        static const Color kLaneCols[4] = {Color{200, 90, 240, 255}, Color{60, 200, 255, 255},
                                           Color{70, 235, 110, 255}, Color{250, 75, 95, 255}};

        // Draw Opponent (side 0) & Player (side 1) strums + scrolling guide notes
        for (int side = 0; side < 2; ++side) {
            const float baseSideX = side == 0 ? 320.0f : 960.0f;
            const Vector2 lbl = xform(baseSideX, 58.0f);
            gfx::text(FontId::Ui, side == 0 ? "OPPONENT" : "PLAYER", lbl.x, lbl.y, 15, fade(kIvory, 0.55f), Align::Center);
            for (int l = 0; l < previewKeys_; ++l) {
                const float gx = baseSideX + (float(l) - (previewKeys_ - 1) * 0.5f) * lw + st.ox[side][l];
                const float gy = 140.0f + st.oy[side][l];
                const Vector2 p = xform(gx, gy);
                const float al = st.alpha[side][l];
                const int dir = gfx::laneDir(previewKeys_, l);
                const Color col = kLaneCols[dir % 4];
                // Guide lane line
                const Vector2 pBot = xform(gx, 660.0f);
                DrawLineEx(p, pBot, 1.0f, fade(col, 0.16f * al));
                // Scrolling sample notes showing scroll speed
                for (int k = 0; k < 4; ++k) {
                    const float bOff = float(k) - float(std::fmod(curBeat_, 1.0));
                    const float ny = gy + bOff * 110.0f * (st.scroll * 0.5f);
                    if (ny < gy || ny > 660.0f) continue;
                    const Vector2 np = xform(gx, ny);
                    DrawCircleV(np, noteSz * 0.30f, fade(col, 0.55f * al));
                }
                // Receptor square rotated by strum angle + camAngle
                const Rectangle rec{p.x, p.y, noteSz, noteSz};
                DrawRectanglePro(rec, {noteSz * 0.5f, noteSz * 0.5f}, st.ang[side][l] + st.camAngle, fade(col, 0.85f * al));
                DrawCircleV(p, noteSz * 0.14f, fade(WHITE, 0.9f * al));
            }
        }

        gfx::text(FontId::Ui,
                  TextFormat("Zoom %.2fx   CamAngle %.1f   Scroll %.2f   Drunk %.0f   Tipsy %.0f   Bounce %.0f",
                             st.camZoom, st.camAngle, st.scroll, st.drunk, st.tipsy, st.bounce),
                  view.x + 10, view.y + view.height - 22, 14, fade(kIvory, 0.72f));

        // Timeline scrubber bar
        const Rectangle bar{area.x + 16, view.y + view.height + 12, area.width - 32, 34};
        DrawRectangleRounded(bar, 0.25f, 6, Color{24, 18, 40, 255});
        DrawRectangleRoundedLinesEx(bar, 0.25f, 6, 1.0f, Color{96, 80, 140, 200});
        for (int i = 0; i < int(events_.size()); ++i) {
            const float fx = float(std::clamp(events_[size_t(i)].beat / std::max(1.0, maxBeat_), 0.0, 1.0));
            const float mx = bar.x + 6 + fx * (bar.width - 12);
            DrawRectangleRec({mx - 2, bar.y + 4, 4, bar.height - 8}, i == sel_ ? kGold : Color{130, 210, 255, 220});
        }
        const float headX = bar.x + 6 + float(std::clamp(curBeat_ / std::max(1.0, maxBeat_), 0.0, 1.0)) * (bar.width - 12);
        DrawRectangleRec({headX - 2, bar.y - 3, 4, bar.height + 6}, WHITE);

        if (ui_.hover(bar) && ui_.click) {
            scrubbing_ = true;
            playing_ = false;
        }
        if (scrubbing_) {
            if (!ui_.down) scrubbing_ = false;
            else {
                const float fx = std::clamp((ui_.mouse.x - (bar.x + 6)) / std::max(1.0f, bar.width - 12), 0.0f, 1.0f);
                curBeat_ = std::round(fx * maxBeat_ * 4.0) * 0.25;
            }
        }
    }
};

}  // namespace

std::unique_ptr<State> makeScriptEditor() { return std::make_unique<ScriptEditorState>(); }
std::unique_ptr<State> makeChartConverter() { return std::make_unique<ChartConverterState>(); }
std::unique_ptr<State> makeEditorMenu() { return std::make_unique<EditorMenuState>(); }
std::unique_ptr<State> makeCharacterEditor() { return std::make_unique<CharacterEditorState>(); }
std::unique_ptr<State> makeStageEditor() { return std::make_unique<StageEditorState>(); }
std::unique_ptr<State> makeEventEditor() { return std::make_unique<EventEditorState>(); }
std::unique_ptr<State> makeModchartEditor() { return std::make_unique<ModchartEditorState>(); }
std::unique_ptr<State> makeSniffEditor() { return std::make_unique<SniffEditorState>(); }

}  // namespace gh
