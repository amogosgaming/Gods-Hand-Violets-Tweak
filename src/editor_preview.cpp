// God's Hand of Principality and Demise. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

#include "editor_preview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "app.hpp"
#include "basegame.hpp"
#include "builtin_events.hpp"
#include "common.hpp"
#include "gfx.hpp"
#include "platform.hpp"
#include "psych_ui.hpp"

namespace gh {

using scene::Character;
using scene::CharacterPtr;
using scene::Value;

namespace {
namespace fs = std::filesystem;

constexpr int kPreviewRtW = 960;
constexpr int kPreviewRtH = 540;

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool isScriptFile(const std::string& name) {
    return endsWithI(name, ".lua") || endsWithI(name, ".hx") || endsWithI(name, ".hscript") || endsWithI(name, ".hxs");
}

std::vector<std::string> luaFilesIn(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(toPath(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && isScriptFile(pathStr(it->path().filename()))) out.push_back(pathStr(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> scriptsNamed(const scene::Files& files, const std::string& base, bool ownOnly = false) {
    std::vector<std::string> out;
    for (const char* ext : {".lua", ".hx", ".hscript", ".hxs"})
        if (std::string f = ownOnly ? files.findOwn(base + ext) : files.find(base + ext); !f.empty()) out.push_back(f);
    return out;
}

bool isPulseForgeStandIn(const std::string& path) {
    std::string code;
    if (!scene::readFile(path, code)) return false;
    return code.substr(0, 1024).find("PulseForge portable compatibility") != std::string::npos;
}

bool extraKeysScript(const std::string& path) {
    std::string head;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        head.resize(16384);
        head.resize(std::fread(head.data(), 1, head.size(), f));
        std::fclose(f);
    }
    return head.find("Extra Keys Script") != std::string::npos ||
           (head.find("maniaData") != std::string::npos && head.find("swagWidth") != std::string::npos);
}

double numOr(const std::string& s, double def) {
    const std::string t = trim(s);
    if (t.empty()) return def;
    char* end = nullptr;
    const double d = std::strtod(t.c_str(), &end);
    return end == t.c_str() || !std::isfinite(d) ? def : d;
}

Value parseLooseValue(const std::string& s) {
    const std::string t = trim(s);
    if (t == "true") return true;
    if (t == "false") return false;
    char* end = nullptr;
    const double d = std::strtod(t.c_str(), &end);
    if (!t.empty() && end && *end == 0 && std::isfinite(d)) return d;
    return s;
}

int whoForEvent(const std::string& v) {
    const std::string l = toLower(trim(v));
    if (l == "bf" || l == "boyfriend") return 0;
    if (l == "gf" || l == "girlfriend") return 2;
    const int n = int(std::lround(numOr(l, 0)));
    return n == 1 ? 0 : n == 2 ? 2 : 1;
}

std::string singAnimName(int keys, int lane, bool alt) {
    static const char* names[4] = {"singLEFT", "singDOWN", "singUP", "singRIGHT"};
    int d = gfx::laneDir(keys, lane);
    if (d == gfx::kCenter) d = 2;
    std::string a = names[clampv(d, 0, 3)];
    if (alt) a += "-alt";
    return a;
}

int sectionIndexAt(const std::vector<PreviewSection>& sections, double ms) {
    if (sections.empty() || ms <= 0) return 0;
    int lo = 0, hi = int(sections.size()) - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (sections[size_t(mid)].startMs <= ms) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

double beatAtMs(const std::vector<PreviewSection>& sections, double ms) {
    if (sections.empty()) return std::max(0.0, ms) * 120.0 / 60000.0;
    const int idx = sectionIndexAt(sections, ms);
    const PreviewSection& sec = sections[size_t(idx)];
    const double bpm = std::max(1.0, sec.bpm);
    return sec.beatStart + (ms - sec.startMs) * bpm / 60000.0;
}

}  // namespace

ChartPreview::ChartPreview() = default;

ChartPreview::~ChartPreview() {
    unload();
    if (rt_.id && IsWindowReady()) UnloadRenderTexture(rt_);
    rt_ = RenderTexture2D{};
}

void ChartPreview::unload() {
    scripts_.reset();
    renderer_.clear();
    scene_.reset();
    loaded_ = false;
    baseStage_ = false;
    status_.clear();
    lastMs_ = -1;
    nextEvent_ = 0;
    lastBeat_ = lastStep_ = lastSection_ = -1;
}

void ChartPreview::placeCharacter(Character& c, int slot) {
    const psych::StageDef& st = scene_->stage;
    const float* g = slot == 0 ? st.bf : slot == 1 ? st.dad : st.gf;
    float gx = g[0], gy = g[1];
    c.scrollX = c.scrollY = 1;
    if (slot == 2 || (slot == 1 && c.name.rfind("gf", 0) == 0)) {
        gx = st.gf[0];
        gy = st.gf[1];
        c.scrollX = c.scrollY = 0.95f;
        if (slot == 1) c.danceEvery = 2;
    }
    c.groupX = gx;
    c.groupY = gy;
    c.x = gx + c.posX;
    c.y = gy + c.posY;
}

CharacterPtr ChartPreview::makeCharacter(const std::string& name, bool isPlayer, int slot) {
    scene::Scene& sc = *scene_;
    auto c = std::make_shared<Character>();
    c->kind = scene::Kind::Character;
    c->name = name;
    c->isPlayer = isPlayer;
    c->tag = slot == 0 ? "boyfriend" : slot == 1 ? "dad" : "gf";
    std::string path = sc.files.find("characters/" + name + ".json");
    if (path.empty()) path = sc.files.find("data/characters/" + name + ".json");
    psych::CharacterDef def;
    bool ok = false;
    std::string err;
    if (!path.empty()) {
        std::string text;
        ok = scene::readFile(path, text) && psych::parseCharacter(text, def, err);
    } else if (const char* base = basegame::characterJson(name)) {
        ok = psych::parseCharacter(base, def, err) && !def.images.empty() &&
             !sc.files.find("images/" + def.images[0] + ".png").empty();
    }
    scene::GraphicPtr g;
    if (ok) {
        std::string keys;
        for (const std::string& im : def.images) keys += (keys.empty() ? "" : ",") + im;
        g = sc.gfx.atlas(keys);
    }
    if (ok && g) {
        c->def = def;
        c->setGraphic(g);
        for (const psych::AnimDef& ad : def.anims) {
            scene::Anim a;
            a.name = ad.anim;
            a.frames = ad.indices.empty() ? psych::framesByPrefix(g->frames, ad.prefix)
                                          : psych::framesByIndices(g->frames, ad.prefix, ad.indices);
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
        c->recalcDance(true);
        c->dance();
    } else {
        if (slot == 2) return nullptr;
        c->builtin = true;
        c->builtinId = isPlayer ? int(CharId::Herald)
                                : int(cfg_.entry && cfg_.entry->builtin ? charFromName(cfg_.entry->opponent) : CharId::Shade);
        c->frameW = c->width = 440;
        c->frameH = c->height = 480;
        c->centerOrigin();
        c->posY = 290;
        const Color cc = isPlayer ? charColor(CharId::Herald) : gfx::rgb(cfg_.entry ? cfg_.entry->color : 0xB48CDC);
        c->barColor = (uint32_t(cc.r) << 16) | (uint32_t(cc.g) << 8) | cc.b;
        c->recalcDance(true);
    }
    placeCharacter(*c, slot);
    return c;
}

void ChartPreview::setupStrums(int keys) {
    if (!scene_) return;
    keys = clampv(keys, 1, kMaxKeys);
    scene_->keys = keys;
    scene_->strums.assign(size_t(2 * keys), scene::Strum{});
    const float y = cfg_.downscroll ? 570.0f : 50.0f;
    const float noteW = std::min(112.0f, 460.0f / float(keys));
    const float oppStart = 92.0f + (448.0f - noteW * float(keys)) * 0.5f;
    const float plStart = 732.0f + (448.0f - noteW * float(keys)) * 0.5f;
    for (int side = 0; side < 2; ++side) {
        const float start = side == 0 ? oppStart : plStart;
        for (int l = 0; l < keys; ++l) {
            scene::Strum& s = scene_->strums[size_t(side * keys + l)];
            s.x = s.defX = start + l * noteW;
            s.y = s.defY = y;
            s.scaleX = s.scaleY = 0.7f * (noteW / 112.0f);
            s.downScroll = cfg_.downscroll;
            s.alpha = 1.0f;
            s.visible = true;
        }
    }
}

void ChartPreview::setDownscroll(bool ds) {
    if (cfg_.downscroll == ds) return;
    cfg_.downscroll = ds;
    if (!scene_) return;
    const float newDefY = ds ? 570.0f : 50.0f;
    for (scene::Strum& s : scene_->strums) {
        const float delta = s.y - s.defY;
        s.defY = newDefY;
        s.y = newDefY + delta;
        s.downScroll = ds;
    }
    if (scripts_) {
        scripts_->setGlobal("downscroll", ds);
        for (int i = 0; i < scene_->keys && i < kMaxKeys; ++i) {
            scripts_->setGlobal(TextFormat("defaultOpponentStrumY%d", i), double(newDefY));
            scripts_->setGlobal(TextFormat("defaultPlayerStrumY%d", i), double(newDefY));
        }
        for (int i = 0; i < 2 * scene_->keys && i < 18; ++i)
            scripts_->setGlobal(TextFormat("defaultStrum%dY", i), double(newDefY));
    }
}

void ChartPreview::setKeys(int keys) {
    keys = clampv(keys, 1, kMaxKeys);
    if (cfg_.keys == keys) return;
    cfg_.keys = keys;
    setupStrums(keys);
    if (scripts_) {
        scripts_->setGlobal("keyCount", double(keys));
        scripts_->setGlobal("playerKeyCount", double(keys));
        scripts_->setGlobal("opponentKeyCount", double(keys));
    }
}

void ChartPreview::setGlobals(const std::vector<PreviewSection>& sections) {
    App& a = app();
    const Settings& s = a.settings;
    ScriptHost& h = *scripts_;
    scene::Scene& sc = *scene_;
    const double bpm = sections.empty() ? 120.0 : std::max(1.0, sections[0].bpm);
    auto set = [&](const char* n, const Value& v) { h.setGlobal(n, v); };
    set("luaDebugMode", false);
    set("luaDeprecatedWarnings", false);
    set("inChartEditor", true);
    set("version", std::string("0.7.3"));
    set("jsVersion", std::string("1.50.0"));
    set("curBpm", bpm);
    set("bpm", bpm);
    set("scrollSpeed", cfg_.speed);
    set("crochet", 60000.0 / bpm);
    set("stepCrochet", 15000.0 / bpm);
    set("songLength", cfg_.lengthMs);
    set("songName", cfg_.title);
    set("songPath", cfg_.entry ? cfg_.entry->songKey : std::string());
    set("startedCountdown", true);
    set("curStage", stageName_);
    set("isStoryMode", false);
    set("difficulty", 1.0);
    set("difficultyName", cfg_.diff);
    set("difficultyPath", toLower(cfg_.diff));
    set("seenCutscene", true);
    set("hasVocals", true);
    set("cameraX", double(sc.followX));
    set("cameraY", double(sc.followY));
    set("screenWidth", double(gfx::W));
    set("screenHeight", double(gfx::H));
    set("curSection", 0.0);
    set("curBeat", 0.0);
    set("curStep", 0.0);
    set("curDecBeat", 0.0);
    set("curDecStep", 0.0);
    set("score", 0.0);
    set("misses", 0.0);
    set("hits", 0.0);
    set("combo", 0.0);
    set("rating", 1.0);
    set("ratingName", std::string("Sick!"));
    set("ratingFC", std::string("SFC"));
    set("inGameOver", false);
    const bool mh = sections.empty() ? true : sections[0].mustHit;
    set("mustHitSection", mh);
    set("altAnim", !sections.empty() && sections[0].altAnim);
    set("gfSection", !sections.empty() && sections[0].gf);
    set("healthGainMult", 1.0);
    set("healthLossMult", 1.0);
    set("playbackRate", 1.0);
    set("botPlay", true);
    set("practice", true);
    for (int i = 0; i < sc.keys && i < kMaxKeys; ++i) {
        const scene::Strum& o = sc.strums[size_t(i)];
        const scene::Strum& p = sc.strums[size_t(sc.keys + i)];
        set(TextFormat("defaultOpponentStrumX%d", i), double(o.x));
        set(TextFormat("defaultOpponentStrumY%d", i), double(o.y));
        set(TextFormat("defaultPlayerStrumX%d", i), double(p.x));
        set(TextFormat("defaultPlayerStrumY%d", i), double(p.y));
    }
    for (int i = 0; i < 2 * sc.keys && i < 18; ++i) {
        set(TextFormat("defaultStrum%dX", i), double(sc.strums[size_t(i)].x));
        set(TextFormat("defaultStrum%dY", i), double(sc.strums[size_t(i)].y));
    }
    set("defaultBoyfriendX", double(sc.stage.bf[0]));
    set("defaultBoyfriendY", double(sc.stage.bf[1]));
    set("defaultOpponentX", double(sc.stage.dad[0]));
    set("defaultOpponentY", double(sc.stage.dad[1]));
    set("defaultGirlfriendX", double(sc.stage.gf[0]));
    set("defaultGirlfriendY", double(sc.stage.gf[1]));
    set("boyfriendName", sc.bf ? sc.bf->name : std::string());
    set("dadName", sc.dad ? sc.dad->name : std::string());
    set("gfName", sc.gf ? sc.gf->name : std::string());
    set("downscroll", cfg_.downscroll);
    set("middlescroll", false);
    set("framerate", double(s.fpsCap > 0 ? s.fpsCap : 240));
    set("ghostTapping", s.ghostTapping);
    set("hideHud", false);
    set("cameraZoomOnBeat", s.camZoom);
    set("flashingLights", s.flashing);
    set("lowQuality", s.lowDetail);
    set("shadersEnabled", s.shaders);
    set("keyCount", double(sc.keys));
    set("playerKeyCount", double(sc.keys));
    set("opponentKeyCount", double(sc.keys));
    set("scrollspeed", cfg_.speed);
    set("mustHit", mh);
}

void ChartPreview::loadScripts(const std::vector<psych::ChartEvent>& events) {
    scene::Scene& sc = *scene_;
    ScriptHost& h = *scripts_;
    auto loadFile = [&](const std::string& f) {
        if (extraKeysScript(f)) return;
        h.load(f);
    };
    for (const char* ext : {".lua", ".hx", ".hscript", ".hxs"})
        for (const std::string& f : sc.files.list("scripts", ext)) loadFile(f);
    const auto stageScripts = scriptsNamed(sc.files, "stages/" + stageName_);
    for (const std::string& f : stageScripts) loadFile(f);
    if (stageScripts.empty() && baseStage_) h.loadString("stages/" + stageName_ + ".lua", basegame::stageLua(stageName_));
    for (Character* c : {sc.gf.get(), sc.dad.get(), sc.bf.get()}) {
        if (!c || c->builtin) continue;
        for (const std::string& f : scriptsNamed(sc.files, "characters/" + c->name))
            if (!h.isLoaded(f)) loadFile(f);
    }
    std::vector<std::string> seen;
    for (const psych::ChartEvent& e : events) {
        if (std::find(seen.begin(), seen.end(), e.name) != seen.end()) continue;
        seen.push_back(e.name);
        auto files = scriptsNamed(sc.files, "custom_events/" + e.name, true);
        if (builtin::engineEvent(e.name))
            files.erase(std::remove_if(files.begin(), files.end(), isPulseForgeStandIn), files.end());
        if (files.empty() && builtin::engineEvent(e.name)) {
            if (const char* code = builtin::eventLua(e.name)) h.loadString("custom_events/" + e.name + ".lua", code);
            continue;
        }
        if (files.empty()) files = scriptsNamed(sc.files, "custom_events/" + e.name);
        for (const std::string& f : files) loadFile(f);
    }
    if (cfg_.entry && !cfg_.entry->dataDir.empty()) {
        for (const std::string& f : luaFilesIn(cfg_.entry->dataDir)) loadFile(f);
    }
}

void ChartPreview::load(const PreviewConfig& cfg, const std::vector<PreviewSection>& sections,
                        const std::vector<psych::ChartEvent>& events) {
    unload();
    cfg_ = cfg;
    speed_ = cfg.speed;
    scene_ = std::make_unique<scene::Scene>();
    scene::Scene& sc = *scene_;

    App& a = app();
    std::vector<std::string> roots;
    if (cfg.entry) {
        roots = cfg.entry->assetRoots;
        if (roots.empty() && !cfg.entry->modRoot.empty()) roots.push_back(cfg.entry->modRoot);
    }
    roots.push_back(a.base + "mods/");
    sc.files.setRoots(std::move(roots));
    if (cfg.entry) sc.files.setSharedRoots(a.library.sharedAssetRootsFor(*cfg.entry));

    auto baseStageAvailable = [&](const std::string& st) {
        const char* probe = basegame::stageProbeImage(st);
        if (!probe) return false;
        std::string_view rest = probe;
        while (!rest.empty()) {
            const size_t bar = rest.find('|');
            if (!sc.files.find("images/" + std::string(rest.substr(0, bar)) + ".png").empty()) return true;
            if (bar == std::string_view::npos) break;
            rest.remove_prefix(bar + 1);
        }
        return false;
    };

    stageName_ = cfg.stage;
    if (stageName_.empty() && cfg.entry) {
        const std::string guess = basegame::stageForSong(cfg.entry->songKey);
        stageName_ = baseStageAvailable(guess) ? guess : "stage";
    }
    if (stageName_.empty()) stageName_ = "stage";
    const std::string playerName = cfg.player.empty() ? "bf" : cfg.player;
    const std::string oppName = cfg.opponent.empty() ? "dad" : cfg.opponent;
    std::string gfName = cfg.gf.empty() ? "gf" : cfg.gf;
    if (cfg.gf.empty() && baseStageAvailable(stageName_)) gfName = basegame::gfForStage(stageName_);

    const std::string stageJson = sc.files.find("stages/" + stageName_ + ".json");
    std::string stageLua = sc.files.find("stages/" + stageName_ + ".lua");
    if (stageLua.empty()) stageLua = sc.files.find("stages/" + stageName_ + ".hx");
    baseStage_ = stageLua.empty() && basegame::stageLua(stageName_) && baseStageAvailable(stageName_);

    if (!stageJson.empty()) {
        std::string text, err;
        if (scene::readFile(stageJson, text)) psych::parseStage(text, sc.stage, err);
    } else if (const char* bs = basegame::stageJson(stageName_); bs && baseStageAvailable(stageName_)) {
        std::string err;
        psych::parseStage(bs, sc.stage, err);
    }
    sc.defaultCamZoom = sc.stage.zoom;
    sc.camGame.zoom = float(sc.stage.zoom);
    sc.cameraSpeed = sc.stage.camSpeed;

    sc.gfMarker = std::make_shared<scene::Sprite>();
    sc.gfMarker->kind = scene::Kind::Marker;
    sc.gfMarker->tag = "gfGroup";
    if (!sc.stage.hideGf) sc.gf = makeCharacter(gfName, false, 2);
    sc.dad = makeCharacter(oppName, false, 1);
    sc.bf = makeCharacter(playerName, true, 0);
    if (sc.dad && sc.gf && sc.dad->name.rfind("gf", 0) == 0) sc.gf->visible = false;

    bool placedChars = false;
    for (const psych::StageObject& o : sc.stage.objects) {
        const std::string t = toLower(o.type);
        if (t == "gf" || t == "gfgroup") {
            sc.order.push_back(sc.gfMarker);
            if (sc.gf) sc.order.push_back(sc.gf);
            placedChars = true;
            continue;
        }
        if (t == "dad" || t == "dadgroup") { if (sc.dad) sc.order.push_back(sc.dad); continue; }
        if (t == "boyfriend" || t == "boyfriendgroup") { if (sc.bf) sc.order.push_back(sc.bf); continue; }
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
            an.hasOffset = true;
            an.offX = ad.offX;
            an.offY = ad.offY;
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
    }
    if (!placedChars) {
        sc.order.push_back(sc.gfMarker);
        if (sc.gf) sc.order.push_back(sc.gf);
        if (sc.dad) sc.order.push_back(sc.dad);
        if (sc.bf) sc.order.push_back(sc.bf);
    }

    // Minimal HUD placeholders so Lua scripts referencing healthBar/scoreTxt/iconP1 don't fail.
    for (const char* name : {"healthBar", "healthBarBG", "iconP1", "iconP2", "scoreTxt", "timeBar", "timeBarBG", "timeTxt", "botplayTxt"}) {
        auto h = std::make_shared<scene::Sprite>();
        h->tag = name;
        h->cam = scene::Cam::Hud;
        h->visible = true;
        sc.hud[name] = h;
    }

    setupStrums(cfg.keys);
    const int focus0 = sections.empty() ? 0 : (sections[0].gf ? 2 : (sections[0].mustHit ? 0 : 1));
    sc.focusOn(focus0);
    sc.camGame.scrollX = sc.followX - gfx::W * 0.5f;
    sc.camGame.scrollY = sc.followY - gfx::H * 0.5f;

    // Built-in character fallback states
    builtinChars_[0].id = CharId::Herald;
    builtinChars_[1].id = cfg.entry && cfg.entry->builtin ? charFromName(cfg.entry->opponent) : CharId::Shade;
    builtinChars_[1].tint = gfx::rgb(cfg.entry ? cfg.entry->color : 0xB48CDC);
    builtinChars_[2].id = CharId::Shade;
    builtinChars_[2].tint = Color{220, 90, 140, 255};

    scripts_ = std::make_unique<ScriptHost>(sc, *this);
    scripts_->callBudget = 0.5;
    scripts_->loadBudget = 3.0;
    if (cfg.entry && !cfg.entry->songKey.empty()) scripts_->songDataDir = "data/" + cfg.entry->songKey + "/";
    setGlobals(sections);
    loadScripts(events);
    scripts_->call("onCreatePost");
    scripts_->call("onStartCountdown");
    scripts_->call("onCountdownStarted");
    scripts_->call("onSongStart");
    sc.camZooming = true;
    renderer_.preload(sc);

    loaded_ = true;
    lastMs_ = -1.0;
    nextEvent_ = 0;
    scrollSpeedMult_ = 1.0;
    scrollSpeedDur_ = 0;
    camBopRate_ = 4;
    camBopMult_ = 1.0;
    camBopEnabled_ = true;
    camTwistIntensity_ = camTwistIntensity2_ = camTwistPhase_ = 0;
    status_ = TextFormat("%d script%s", int(scripts_->count()), scripts_->count() == 1 ? "" : "s");
}

void ChartPreview::resetTimeline(double curMs, const std::vector<PreviewSection>& sections,
                                 const std::vector<psych::ChartEvent>& events) {
    if (!scene_) return;
    scene::Scene& sc = *scene_;
    sc.camGame.zoom = float(sc.defaultCamZoom);
    sc.camGame.angle = 0;
    sc.camGame.shakeTime = 0;
    sc.camGame.flashTime = 0;
    sc.camGame.fadeTime = 0;
    sc.camGame.fadeAlpha = 0;
    sc.camHud.zoom = 1.0f;
    sc.camHud.angle = 0;
    sc.camHud.shakeTime = 0;
    sc.camHud.flashTime = 0;
    sc.camForced = false;
    scrollSpeedMult_ = 1.0;
    scrollSpeedDur_ = 0;
    camTwistIntensity_ = camTwistIntensity2_ = camTwistPhase_ = 0;
    for (auto& sa : strumAnim_) {
        sa.startedAt = -10.0;
        sa.resetTimer = 0.0;
    }
    const int secIdx = sectionIndexAt(sections, curMs);
    lastSection_ = secIdx;
    const double beat = beatAtMs(sections, curMs);
    lastBeat_ = int(std::floor(beat));
    lastStep_ = int(std::floor(beat * 4.0));
    if (!sections.empty()) {
        const PreviewSection& sec = sections[size_t(secIdx)];
        sc.focusOn(sec.gf ? 2 : (sec.mustHit ? 0 : 1));
    }
    auto it = std::lower_bound(events.begin(), events.end(), curMs,
                               [](const psych::ChartEvent& e, double v) { return e.timeMs < v; });
    nextEvent_ = size_t(it - events.begin());
}

void ChartPreview::sync(double curMs, double dt, bool playing, double speed, int keys,
                        const std::vector<PreviewSection>& sections, const std::vector<PreviewNote>& notes,
                        const std::vector<psych::ChartEvent>& events) {
    if (!loaded_ || !scene_ || !scripts_) return;
    dt = clampv(dt, 0.0, 0.05);
    animNow_ += dt;
    curMs_ = curMs;
    speed_ = speed;
    if (keys != cfg_.keys) setKeys(keys);

    const int secIdx = sectionIndexAt(sections, curMs);
    const PreviewSection defSec{};
    const PreviewSection& sec = sections.empty() ? defSec : sections[size_t(secIdx)];
    curBpm_ = std::max(1.0, sec.bpm);
    curStepMs_ = 15000.0 / curBpm_;

    // Backward seek or initial sync
    if (lastMs_ < 0 || curMs < lastMs_ - 5.0) {
        resetTimeline(curMs, sections, events);
        lastMs_ = curMs;
    }

    const double beatF = beatAtMs(sections, curMs);
    const double stepF = beatF * 4.0;
    const int beat = int(std::floor(beatF));
    const int step = int(std::floor(stepF));

    scripts_->setGlobal("curBpm", curBpm_);
    scripts_->setGlobal("crochet", 60000.0 / curBpm_);
    scripts_->setGlobal("stepCrochet", curStepMs_);
    scripts_->setGlobal("curDecBeat", beatF);
    scripts_->setGlobal("curDecStep", stepF);
    scripts_->setGlobal("curBeat", double(beat));
    scripts_->setGlobal("curStep", double(step));
    scripts_->setGlobal("curSection", double(secIdx));
    scripts_->setGlobal("mustHitSection", sec.mustHit);
    scripts_->setGlobal("mustHit", sec.mustHit);
    scripts_->setGlobal("gfSection", sec.gf);
    scripts_->setGlobal("altAnim", sec.altAnim);
    scripts_->setGlobal("scrollSpeed", speed_ * scrollSpeedMult_);
    scripts_->setGlobal("cameraX", double(scene_->followX));
    scripts_->setGlobal("cameraY", double(scene_->followY));

    // Fire section / beat / step callbacks when advancing forward
    if (secIdx != lastSection_) {
        lastSection_ = secIdx;
        onSection(secIdx, sec);
    }
    if (step > lastStep_) {
        const int count = std::min(step - lastStep_, 16);
        for (int k = step - count + 1; k <= step; ++k) onStep(k);
        lastStep_ = step;
    }
    if (beat > lastBeat_) {
        const int count = std::min(beat - lastBeat_, 8);
        for (int k = beat - count + 1; k <= beat; ++k) onBeat(k, curMs);
        lastBeat_ = beat;
    }

    // Fire chart events in (lastMs_, curMs]
    const double advance = curMs - lastMs_;
    while (nextEvent_ < events.size() && events[nextEvent_].timeMs <= curMs) {
        const psych::ChartEvent& e = events[nextEvent_++];
        if (advance <= 400.0 || e.name == "Change Character" || e.name == "Set Property" ||
            e.name == "Camera Follow Pos" || e.name == "Set Camera Zoom" || e.name == "Change Scroll Speed" ||
            e.name == "Alt Idle Animation") {
            triggerEvent(e.name, e.v1, e.v2);
        }
    }

    // Trigger note hits & strum animations when advancing cleanly
    if (advance > 0.0 && advance <= 160.0) {
        auto lo = std::upper_bound(notes.begin(), notes.end(), lastMs_,
                                   [](double v, const PreviewNote& n) { return v < n.t; });
        int fired = 0;
        for (auto it = lo; it != notes.end() && it->t <= curMs && fired < 64; ++it, ++fired) {
            hitPreviewNote(*it, curStepMs_, sec);
        }
    }

    // Sustain hold refreshes (keep strums & singers alive while a hold crosses curMs)
    if (playing) {
        auto lo = std::lower_bound(notes.begin(), notes.end(), curMs - 15000.0,
                                   [](const PreviewNote& n, double v) { return n.t < v; });
        for (auto it = lo; it != notes.end() && it->t <= curMs; ++it) {
            if (it->len > 0 && curMs < it->t + it->len && it->col < strumAnim_.size() && !it->mine) {
                strumAnim_[it->col].resetTimer = std::max(strumAnim_[it->col].resetTimer, 0.09);
            }
        }
    }

    // Update strum confirm timers
    for (auto& sa : strumAnim_) {
        if (sa.resetTimer > 0.0) {
            sa.resetTimer = std::max(0.0, sa.resetTimer - dt);
        }
    }

    // Update scroll speed tween
    if (scrollSpeedDur_ > 0) {
        scrollSpeedT_ += dt;
        const double f = clampv(scrollSpeedT_ / scrollSpeedDur_, 0.0, 1.0);
        scrollSpeedMult_ = scrollSpeedFrom_ + (scrollSpeedTo_ - scrollSpeedFrom_) * f;
        if (f >= 1.0) scrollSpeedDur_ = 0;
    }

    // Camera twist oscillation
    if (std::fabs(camTwistIntensity_) > 1e-4) {
        camTwistPhase_ += dt * (curBpm_ / 60.0) * 3.1415926535;
        scene_->camGame.angle = float(std::sin(camTwistPhase_) * camTwistIntensity_ * 2.0);
        scene_->camHud.angle = float(std::sin(camTwistPhase_) * camTwistIntensity2_ * 1.2);
    }

    scripts_->call("onUpdate", {dt});
    scene_->update(dt, curStepMs_, false);
    scripts_->dispatchCompletions();
    scripts_->call("onUpdatePost", {dt});
    lastMs_ = curMs;
}

void ChartPreview::onSection(int secIdx, const PreviewSection& sec) {
    if (!scene_ || !scripts_) return;
    if (!scene_->camForced) scene_->focusOn(sec.gf ? 2 : (sec.mustHit ? 0 : 1));
    scripts_->call("onSectionHit");
    (void)secIdx;
}

void ChartPreview::onStep(int step) {
    if (!scripts_) return;
    scripts_->call("onStepHit");
    (void)step;
}

void ChartPreview::onBeat(int beat, double curMs) {
    if (!scene_ || !scripts_) return;
    scene::Scene& sc = *scene_;
    if (camBopEnabled_ && sc.camZooming && app().settings.camZoom && camBopRate_ > 0 && beat % camBopRate_ == 0) {
        sc.camGame.zoom += float(0.015 * sc.camZoomingMult * camBopMult_);
        sc.camHud.zoom += float(0.03 * sc.camZoomingMult * camBopMult_);
    }
    for (int i = 0; i < 3; ++i) builtinChars_[i].beat(animNow_);
    auto danceIfReady = [&](Character* c) {
        if (!c || c->specialAnim || c->stunned || c->skipDance) return;
        if (c->now - c->lastDanceAt < 0.12) return;
        const int every = c == sc.gf.get() ? std::max(1, int(std::lround(sc.gfSpeed))) : std::max(1, c->danceEvery);
        if (beat % every != 0) return;
        const std::string cur = c->curAnimName();
        if (cur.rfind("sing", 0) == 0 && !c->finished) return;
        c->dance();
    };
    danceIfReady(sc.gf.get());
    danceIfReady(sc.dad.get());
    danceIfReady(sc.bf.get());
    scripts_->call("onBeatHit");
    (void)curMs;
}

void ChartPreview::hitPreviewNote(const PreviewNote& n, double stepMs, const PreviewSection& sec) {
    if (!scene_ || n.mine) return;
    const int keys = std::max(1, cfg_.keys);
    const bool player = n.col >= keys;
    const int lane = n.col % keys;
    const double dur = clampv(stepMs * 1.25 / 1000.0, 0.08, 0.28);
    if (n.col < strumAnim_.size()) {
        strumAnim_[n.col].startedAt = animNow_;
        strumAnim_[n.col].resetTimer = std::max(dur, n.len > 0 ? 0.14 : dur);
    }

    const bool noAnim = equalsI(n.type, "No Animation");
    const bool gfSing = equalsI(n.type, "GF Sing") || sec.gf;
    const bool alt = equalsI(n.type, "Alt Animation") || (!player && sec.altAnim);
    if (!noAnim) {
        Character* c = gfSing && scene_->gf ? scene_->gf.get() : (player ? scene_->bf.get() : scene_->dad.get());
        const int bIdx = gfSing ? 2 : (player ? 0 : 1);
        int dir = gfx::laneDir(keys, lane);
        if (dir == gfx::kCenter) dir = 2;
        builtinChars_[bIdx].sing(clampv(dir, 0, 3), animNow_, clampv(stepMs * 0.0044, 0.16, 0.65));
        if (c && !c->builtin) {
            std::string anim = singAnimName(keys, lane, alt);
            if (!c->hasAnim(anim)) anim = singAnimName(keys, lane, false);
            if (c->hasAnim(anim)) {
                const bool rapidSame = c->curAnimName() == anim && (c->now - c->lastAnimAt) < 0.045;
                if (!rapidSame) c->playAnim(anim, true);
                c->holdTimer = 0;
            }
        }
        if (equalsI(n.type, "Hey!") && c && !c->builtin && c->hasAnim("hey")) {
            c->playAnim("hey", true);
            c->specialAnim = true;
            c->heyTimer = 0.6;
        }
    }
    if (scripts_ && (app().settings.luaNoteCallbacks || !n.type.empty())) {
        scripts_->call(player ? "goodNoteHit" : "opponentNoteHit", {0.0, double(lane), n.type, false});
    }
}

void ChartPreview::triggerEvent(const std::string& name, const std::string& v1, const std::string& v2) {
    handleBuiltInEvent(name, v1, v2);
    if (scripts_) scripts_->call("onEvent", {name, v1, v2});
}

void ChartPreview::handleBuiltInEvent(const std::string& name, const std::string& v1, const std::string& v2) {
    if (!scene_) return;
    scene::Scene& sc = *scene_;
    if (name == "Add Camera Zoom") {
        if (app().settings.camZoom) {
            sc.camGame.zoom += float(numOr(v1, 0.015));
            sc.camHud.zoom += float(numOr(v2, 0.03));
        }
    } else if (name == "Set Camera Zoom") {
        const std::string l = toLower(trim(v1));
        sc.defaultCamZoom = (l.empty() || l == "default") ? sc.stage.zoom : clampv(numOr(v1, sc.stage.zoom), 0.1, 8.0);
    } else if (name == "Tween Camera Zoom") {
        const std::string l = toLower(trim(v1));
        const double target = (l.empty() || l == "default") ? sc.stage.zoom : clampv(numOr(v1, sc.stage.zoom), 0.1, 8.0);
        double dur = 0.5;
        std::string ease = "linear";
        const size_t comma = v2.find(',');
        if (comma != std::string::npos) {
            dur = numOr(v2.substr(0, comma), 0.5);
            ease = trim(v2.substr(comma + 1));
        } else if (!trim(v2).empty()) {
            dur = numOr(v2, 0.5);
        }
        sc.defaultCamZoom = target;
        scene::Tween tw;
        tw.tag = "__preview_camzoom";
        tw.target = "camGame";
        tw.fields = {{"zoom", double(sc.camGame.zoom), target, true}};
        tw.dur = std::max(0.01, dur);
        tw.ease = ease;
        sc.tween(std::move(tw));
    } else if (name == "Camera Bopping") {
        camBopRate_ = std::max(1, int(std::lround(numOr(v1, 4.0))));
        camBopMult_ = numOr(v2, 1.0);
    } else if (name == "Enable Camera Bop") {
        camBopEnabled_ = true;
    } else if (name == "Disable Camera Bop") {
        camBopEnabled_ = false;
    } else if (name == "Camera Twist") {
        const double i1 = numOr(v1, 0.0);
        const double i2 = numOr(v2, 0.0);
        camTwistIntensity_ = i2 == 0.0 ? 0.0 : i1;
        camTwistIntensity2_ = i2;
        if (camTwistIntensity_ == 0.0) {
            sc.camGame.angle = 0;
            sc.camHud.angle = 0;
        }
    } else if (name == "Hey!") {
        const std::string who = toLower(trim(v1));
        const double dur = std::max(0.1, numOr(v2, 0.6));
        auto doHey = [&](Character* c, const char* anim) {
            if (c && !c->builtin && c->hasAnim(anim)) {
                c->playAnim(anim, true);
                c->specialAnim = true;
                c->heyTimer = dur;
            }
        };
        if (who.empty() || who == "bf" || who == "boyfriend" || who == "0") doHey(sc.bf.get(), "hey");
        if (who.empty() || who == "gf" || who == "girlfriend" || who == "1") doHey(sc.gf.get(), "cheer");
    } else if (name == "Set GF Speed") {
        sc.gfSpeed = clampv(numOr(v1, 1.0), 1.0, 16.0);
    } else if (name == "Play Animation") {
        const int who = whoForEvent(v2);
        Character* c = who == 0 ? sc.bf.get() : who == 2 ? sc.gf.get() : sc.dad.get();
        if (c && !c->builtin && c->hasAnim(v1)) {
            c->playAnim(v1, true);
            c->specialAnim = true;
        }
    } else if (name == "Alt Idle Animation") {
        const int who = whoForEvent(v1);
        Character* c = who == 0 ? sc.bf.get() : who == 2 ? sc.gf.get() : sc.dad.get();
        if (c) {
            c->idleSuffix = v2;
            c->recalcDance(false);
        }
    } else if (name == "Camera Follow Pos") {
        if (trim(v1).empty() && trim(v2).empty()) {
            sc.camForced = false;
        } else {
            sc.followX = float(numOr(v1, sc.followX));
            sc.followY = float(numOr(v2, sc.followY));
            sc.camForced = true;
        }
    } else if (name == "Screen Shake") {
        auto parseShake = [](const std::string& s, float& dur, float& amp) {
            const size_t c = s.find(',');
            if (c == std::string::npos) return false;
            dur = float(numOr(s.substr(0, c), 0.0));
            amp = float(numOr(s.substr(c + 1), 0.0));
            return dur > 0 && amp > 0;
        };
        float d = 0, a = 0;
        if (parseShake(v1, d, a)) { sc.camGame.shakeTime = d; sc.camGame.shakeIntensity = a; }
        if (parseShake(v2, d, a)) { sc.camHud.shakeTime = d; sc.camHud.shakeIntensity = a; }
    } else if (name == "Change Character") {
        changeCharacter(whoForEvent(v1), v2);
    } else if (name == "Change Scroll Speed") {
        const double mult = clampv(numOr(v1, 1.0), 0.1, 10.0);
        const double dur = std::max(0.0, numOr(v2, 0.0));
        if (dur <= 0.001) {
            scrollSpeedMult_ = mult;
            scrollSpeedDur_ = 0;
        } else {
            scrollSpeedFrom_ = scrollSpeedMult_;
            scrollSpeedTo_ = mult;
            scrollSpeedDur_ = dur;
            scrollSpeedT_ = 0;
        }
    } else if (name == "Set Property") {
        if (!v1.empty()) sc.setAnyProp(v1, parseLooseValue(v2));
    }
}

void ChartPreview::changeCharacter(int who, const std::string& name) {
    if (!scene_ || name.empty()) return;
    scene::Scene& sc = *scene_;
    CharacterPtr& slot = who == 0 ? sc.bf : who == 2 ? sc.gf : sc.dad;
    CharacterPtr next = makeCharacter(name, who == 0, who);
    if (!next) return;
    if (slot) {
        auto it = std::find(sc.order.begin(), sc.order.end(), slot);
        if (it != sc.order.end()) *it = next;
        else sc.order.push_back(next);
    } else {
        sc.order.push_back(next);
    }
    slot = next;
    renderer_.preload(sc);
}

bool ChartPreview::getGameProp(std::string_view path, Value& out) {
    if (!scene_) return false;
    if (path == "songSpeed") { out = speed_ * scrollSpeedMult_; return true; }
    if (path == "health") { out = 1.0; return true; }
    if (path == "songScore" || path == "score") { out = 0.0; return true; }
    if (path == "songMisses" || path == "misses") { out = 0.0; return true; }
    if (path == "combo") { out = 0.0; return true; }
    if (path == "botPlay" || path == "cpuControlled") { out = true; return true; }
    if (path == "inChartEditor") { out = true; return true; }
    if (path == "defaultCamZoom") { out = scene_->defaultCamZoom; return true; }
    if (path == "camZooming") { out = scene_->camZooming; return true; }
    if (path == "camZoomingMult") { out = scene_->camZoomingMult; return true; }
    if (path == "camZoomingDecay") { out = scene_->camZoomingDecay; return true; }
    if (path == "cameraSpeed") { out = scene_->cameraSpeed; return true; }
    if (path == "gfSpeed") { out = scene_->gfSpeed; return true; }
    if (path == "ClientPrefs.data.downScroll" || path == "ClientPrefs.downScroll") { out = cfg_.downscroll; return true; }
    if (path == "ClientPrefs.data.middleScroll" || path == "ClientPrefs.middleScroll") { out = false; return true; }
    if (path == "ClientPrefs.data.flashing" || path == "ClientPrefs.flashing") { out = app().settings.flashing; return true; }
    if (path == "ClientPrefs.data.shaders" || path == "ClientPrefs.shaders") { out = app().settings.shaders; return true; }
    if (path == "ClientPrefs.data.lowQuality" || path == "ClientPrefs.lowQuality") { out = app().settings.lowDetail; return true; }
    return false;
}

bool ChartPreview::setGameProp(std::string_view path, const Value& v) {
    if (!scene_) return false;
    if (path == "defaultCamZoom") { scene_->defaultCamZoom = clampv(scene::toNum(v, 1.0), 0.1, 8.0); return true; }
    if (path == "camZooming") { scene_->camZooming = scene::toBool(v); return true; }
    if (path == "camZoomingMult") { scene_->camZoomingMult = scene::toNum(v, 1.0); return true; }
    if (path == "camZoomingDecay") { scene_->camZoomingDecay = scene::toNum(v, 1.0); return true; }
    if (path == "cameraSpeed") { scene_->cameraSpeed = scene::toNum(v, 1.0); return true; }
    if (path == "gfSpeed") { scene_->gfSpeed = std::max(1.0, scene::toNum(v, 1.0)); return true; }
    if (path == "songSpeed") {
        const double sp = std::max(0.1, scene::toNum(v, speed_));
        scrollSpeedMult_ = sp / std::max(0.1, speed_);
        return true;
    }
    return false;
}

void ChartPreview::debugPrint(const std::string&, uint32_t) {}
void ChartPreview::debugNotice(const std::string&, bool, bool) {}

std::vector<std::string> ChartPreview::singAnimations() {
    std::vector<std::string> out;
    const int k = std::max(1, cfg_.keys);
    for (int l = 0; l < k; ++l) out.push_back(singAnimName(k, l, false));
    return out;
}

bool ChartPreview::shadersEnabled() { return app().settings.shaders; }

void ChartPreview::drawBuiltinChar(const Character& c, Vector2 feet, float scale) const {
    const int idx = c.tag == "boyfriend" ? 0 : c.tag == "gf" ? 2 : 1;
    drawCharacter(builtinChars_[idx], feet, scale * 0.85f, animNow_, c.isPlayer);
}

void ChartPreview::drawHudNotes(double curMs, double speed, int keys, const std::vector<PreviewNote>& notes) const {
    if (!scene_) return;
    keys = clampv(keys, 1, kMaxKeys);
    const int cols = 2 * keys;
    const bool ds = cfg_.downscroll;
    const PsychUi& ui = psychUi();
    const bool usePsych = ui.active() && keys == 4;
    const double effSpeed = std::max(0.1, speed * scrollSpeedMult_);
    const float noteW = std::min(112.0f, 460.0f / float(keys));
    const double pp = 0.45 * effSpeed * (noteW / 112.0);
    const float dirSign = ds ? -1.0f : 1.0f;

    // Draw strums / receptors
    for (int c = 0; c < cols && c < int(scene_->strums.size()); ++c) {
        const scene::Strum& st = scene_->strums[size_t(c)];
        if (!st.visible || st.alpha <= 0.01f) continue;
        const int dir = gfx::laneDir(keys, c % keys);
        const Vector2 center{st.x + noteW * 0.5f, st.y + noteW * 0.5f};
        const bool lit = c < int(strumAnim_.size()) && strumAnim_[size_t(c)].resetTimer > 0.0;
        const double age = lit ? std::max(0.0, animNow_ - strumAnim_[size_t(c)].startedAt) : 0.0;
        bool drawn = false;
        if (usePsych) {
            if (lit) ui.beginDir(dir);
            drawn = ui.strum(dir, center, noteW * 0.92f, lit ? 2 : 0, st.alpha, st.angle, age);
            if (lit) ui.endDir();
        }
        if (!drawn) {
            if (lit) {
                gfx::glow(center, noteW * 0.85f, gfx::fade(gfx::dirColor(dir), 0.55f * st.alpha));
                gfx::drawNote(dir, center, noteW * 0.92f, st.alpha, 1.06f);
            } else {
                gfx::drawReceptor(dir, center, noteW * 0.92f, gfx::fade(WHITE, st.alpha));
            }
        }
    }

    // Draw sustains and notes within visible window
    const double lookAhead = 800.0 / std::max(0.05, pp);
    const double lookBehind = 250.0 / std::max(0.05, pp);
    auto lo = std::lower_bound(notes.begin(), notes.end(), curMs - 15000.0,
                               [](const PreviewNote& n, double v) { return n.t < v; });
    auto hi = std::upper_bound(lo, notes.end(), curMs + lookAhead,
                               [](double v, const PreviewNote& n) { return v < n.t; });
    // Sustains first
    for (auto it = lo; it != hi; ++it) {
        const PreviewNote& n = *it;
        if (n.len <= 0 || n.t + n.len < curMs - lookBehind || n.col >= scene_->strums.size()) continue;
        const scene::Strum& st = scene_->strums[n.col];
        if (!st.visible || st.alpha <= 0.01f) continue;
        const float cx = st.x + noteW * 0.5f;
        const float recY = st.y + noteW * 0.5f;
        const float y0 = recY + dirSign * float(std::max(0.0, n.t - curMs) * pp);
        const float y1 = recY + dirSign * float((n.t + n.len - curMs) * pp);
        const int dir = gfx::laneDir(keys, n.col % keys);
        const int sdir = dir == gfx::kCenter ? gfx::kUp : dir;
        bool drawn = false;
        if (usePsych) {
            ui.beginDir(sdir);
            drawn = ui.hold(sdir, cx, y0, y1, noteW * 0.32f, st.alpha * 0.85f);
            ui.endDir();
        }
        if (!drawn) {
            const float ya = std::min(y0, y1), yb = std::max(y0, y1);
            gfx::drawSustain(sdir, cx, ya, yb, noteW * 0.32f, st.alpha * 0.85f, !ds);
        }
    }
    // Note heads
    float lastY[2 * kMaxKeys];
    std::fill(std::begin(lastY), std::end(lastY), -1e9f);
    int drawnCount = 0;
    for (auto it = lo; it != hi && drawnCount < 4000; ++it) {
        const PreviewNote& n = *it;
        if (n.t < curMs - 1.0 || n.col >= scene_->strums.size()) continue;
        const scene::Strum& st = scene_->strums[n.col];
        if (!st.visible || st.alpha <= 0.01f) continue;
        const float cx = st.x + noteW * 0.5f;
        const float recY = st.y + noteW * 0.5f;
        const float y = recY + dirSign * float((n.t - curMs) * pp);
        if (y < -noteW || y > gfx::H + noteW) continue;
        if (std::fabs(y - lastY[n.col]) < 2.0f) continue;
        lastY[n.col] = y;
        ++drawnCount;
        const int dir = gfx::laneDir(keys, n.col % keys);
        if (n.mine) {
            DrawCircleV({cx, y}, noteW * 0.34f, Color{220, 40, 60, 255});
            continue;
        }
        bool drawn = false;
        if (usePsych) {
            ui.beginDir(dir);
            drawn = ui.note(dir, {cx, y}, noteW * 0.92f, st.alpha, st.angle);
            ui.endDir();
        }
        if (!drawn) gfx::drawNote(dir, {cx, y}, noteW * 0.92f, st.alpha);
    }
}

void ChartPreview::render(double curMs, double speed, int keys, const std::vector<PreviewNote>& notes) {
    if (!loaded_ || !scene_ || !IsWindowReady()) return;
    if (!rt_.id) {
        rt_ = LoadRenderTexture(kPreviewRtW, kPreviewRtH);
        if (!rt_.id) return;
        SetTextureFilter(rt_.texture, TEXTURE_FILTER_BILINEAR);
    }
    scene::Scene& sc = *scene_;
    renderer_.prepareShaderLayers(sc);

    gfx::setRenderTargetSize(kPreviewRtW, kPreviewRtH);
    gfx::beginTexture(rt_);
    ClearBackground(Color{16, 12, 26, 255});
    gfx::beginCanvas();

    auto noMarker = [](const scene::Sprite&) {};
    auto drawBuiltin = [&](const Character& c, Vector2 feet, float scale) { drawBuiltinChar(c, feet, scale); };

    // Stage + characters (Cam::Game)
    if (sc.camGame.visible && sc.camGame.alpha > 0.001f) {
        renderer_.drawCamera(sc, scene::Cam::Game, noMarker, drawBuiltin);
        renderer_.drawEffects(sc.camGame);
    }

    // HUD sprites + notes (Cam::Hud)
    if (sc.camHud.visible && sc.camHud.alpha > 0.001f) {
        renderer_.drawCamera(sc, scene::Cam::Hud, noMarker, drawBuiltin);
        const float hz = sc.camHud.zoom;
        const Vector2 center{gfx::W * 0.5f - sc.camHud.x - sc.camHud.shakeX,
                             gfx::H * 0.5f - sc.camHud.y - sc.camHud.shakeY};
        if (std::fabs(hz - 1.0f) > 0.001f || std::fabs(sc.camHud.angle) > 0.001f ||
            std::fabs(sc.camHud.x + sc.camHud.shakeX) > 0.1f || std::fabs(sc.camHud.y + sc.camHud.shakeY) > 0.1f) {
            gfx::endCanvas();
            gfx::beginCanvas(hz, center, sc.camHud.angle);
            drawHudNotes(curMs, speed, keys, notes);
            gfx::endCanvas();
            gfx::beginCanvas();
        } else {
            drawHudNotes(curMs, speed, keys, notes);
        }
        renderer_.drawEffects(sc.camHud);
    }

    // Top overlay (Cam::Other)
    if (sc.camOther.visible && sc.camOther.alpha > 0.001f) {
        renderer_.drawCamera(sc, scene::Cam::Other, noMarker, drawBuiltin);
        renderer_.drawEffects(sc.camOther);
    }

    gfx::endCanvas();
    gfx::endTexture();
    gfx::setRenderTargetSize(0, 0);
}

void ChartPreview::drawTo(Rectangle dest) const {
    if (!rt_.id) return;
    DrawTexturePro(rt_.texture, {0, 0, float(rt_.texture.width), -float(rt_.texture.height)}, dest, {0, 0}, 0, WHITE);
}

}  // namespace gh
