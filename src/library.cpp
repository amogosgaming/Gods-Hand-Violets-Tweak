// Astralis Engine. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

#include "library.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>

#include "chart_iutk.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "json.hpp"
#include "mod_install.hpp"

namespace gh {

namespace fs = std::filesystem;

static fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

static std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

static std::vector<fs::path> listDir(const fs::path& dir, bool wantDirs) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const bool isDir = it->is_directory(e2);
        if (isDir == wantDirs) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Library::scan(const std::string& baseDir, const std::function<void(const std::string&, double)>& progress) {
    songs_.clear();
    assets_ = SongScan();
    for (const SongDef& def : builtinSongs()) {
        SongEntry e;
        e.id = "builtin:" + def.id;
        e.title = def.title;
        e.subtitle = def.blurb;
        e.builtin = true;
        e.def = &def;
        e.diffs = def.diffs;
        e.color = def.color;
        e.opponent = def.opponent;
        songs_.push_back(e);
    }
    const size_t firstCustom = songs_.size();
    const fs::path base = toPath(baseDir);
    installReport_.clear();
    installModZips(pathStr(base / "mods"), installReport_, progress);

    scanSongsDir(pathStr(base / "songs"), assets_);
    scanModsDir(pathStr(base / "mods"), assets_);
    // Bundled Psych Engine shared assets (characters, stages, icons, sounds)
    // sit in assets/shared/ and assets/ so Tutorial, Week 1 and mods find them.
    {
        std::error_code ec;
        const fs::path sharedDir = base / "assets" / "shared";
        const fs::path assetsDir = base / "assets";
        if (fs::is_directory(sharedDir, ec)) assets_.sharedRoots.push_back(pathStr(sharedDir) + "/");
        if (fs::is_directory(assetsDir, ec)) assets_.sharedRoots.push_back(pathStr(assetsDir) + "/");
    }
    auto bundledOrder = [](const SongEntry& e) -> int {
        if (!e.pack.empty()) return 100;
        const std::string k = toLower(e.songKey);
        if (k == "tutorial") return 0;
        if (k == "bopeebo") return 1;
        if (k == "fresh") return 2;
        if (k == "dad-battle" || k == "dadbattle") return 3;
        return 50;
    };
    for (SongEntry& e : assets_.songs) {
        const int ord = bundledOrder(e);
        if (ord == 0) {
            e.subtitle = "Tutorial";
            e.color = 0xA5004D;
        } else if (ord >= 1 && ord <= 3) {
            e.subtitle = "Week 1 - Daddy Dearest";
            e.color = 0x9271FD;
        }
        songs_.push_back(std::move(e));
    }
    assets_.songs.clear();
    std::sort(songs_.begin() + std::ptrdiff_t(firstCustom), songs_.end(), [&](const SongEntry& a, const SongEntry& b) {
        const int oa = bundledOrder(a), ob = bundledOrder(b);
        if (oa != ob) return oa < ob;
        return toLower(a.title) < toLower(b.title);
    });
    customCount_ = int(songs_.size() - firstCustom);
    report_ = std::to_string(customCount_) + " song" + (customCount_ == 1 ? "" : "s") + " found";
}

const SongEntry* Library::find(const std::string& id) const {
    for (const SongEntry& e : songs_)
        if (e.id == id) return &e;
    return nullptr;
}

Library::BuiltinAudio Library::builtinAudio(const SongDef& def) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (cache_.id == def.id && cache_.comp) return cache_;
    BuiltinAudio a;
    a.id = def.id;
    a.comp = compose(def, true);
    auto inst = std::make_shared<Pcm>();
    inst->channels = 2;
    inst->data = std::move(a.comp->inst);
    auto vp = std::make_shared<Pcm>();
    vp->channels = 1;
    vp->data = std::move(a.comp->voiceP);
    auto vo = std::make_shared<Pcm>();
    vo->channels = 1;
    vo->data = std::move(a.comp->voiceO);
    a.inst = inst;
    a.vp = vp;
    a.vo = vo;
    cache_ = a;
    return a;
}

// A song's track: playable at once, decoded while it plays.
static PcmPtr streamOrNull(const std::string& path, double startFraction = 0.0) {
    return loadSongAudio(path, startFraction);
}

Prepared Library::prepare(const SongEntry& e, const std::string& diff, bool mirror, bool extraKeys) {
    Prepared p;
    if (e.builtin && e.def) {
        BuiltinAudio a = builtinAudio(*e.def);
        buildChart(*e.def, *a.comp, diff, p.chart);
        p.inst = a.inst;
        p.voiceP = a.vp;
        p.voiceO = a.vo;
    } else {
        size_t idx = 0;
        for (size_t i = 0; i < e.diffs.size(); ++i)
            if (e.diffs[i] == diff) idx = i;
        if (idx >= e.chartPaths.size()) { p.error = "no chart for difficulty " + diff; return p; }
        std::string meta;
        if (!e.metaPath.empty()) readTextFile(e.metaPath, meta);
        std::string err;
        if (!loadChartFile(e.chartPaths[idx], diff, p.chart, err, meta, 0, extraKeys)) { p.error = err; return p; }
        if (p.chart.title.empty()) p.chart.title = e.title;
        p.chart.difficulty = diff;
        // Psych keeps events in a separate events.json next to the charts.
        // iutk .bin charts already load sibling events.json inside loadIutkChart().
        if (!isIutkChart(e.chartPaths[idx])) {
            for (const fs::path& f : listDir(toPath(e.dataDir), false)) {
                if (!equalsI(pathStr(f.filename()), "events.json")) continue;
                std::string evText;
                if (readTextFile(pathStr(f), evText)) {
                    psych::parseEvents(evText, p.chart.events);
                    std::stable_sort(p.chart.events.begin(), p.chart.events.end(),
                                     [](const psych::ChartEvent& a, const psych::ChartEvent& b) { return a.timeMs < b.timeMs; });
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(customMtx_);
            if (customCacheKey_ != e.id) {
                customCacheKey_ = e.id;
                if (customInstKey_ != e.id) customInst_ = streamOrNull(e.inst);
                customInstKey_ = e.id;
                customVoices_ = streamOrNull(e.voices);
                customVp_ = streamOrNull(e.voicesP);
                customVo_ = streamOrNull(e.voicesO);
            }
            p.inst = customInst_;
            if (customVp_ || customVo_) {
                p.voiceP = customVp_;
                p.voiceO = customVo_;
            } else {
                p.voiceP = customVoices_;  // one shared vocal track: muted on any miss
            }
        }
    }
    if (mirror) {
        p.chart.mirror();
        if (p.voiceO) std::swap(p.voiceP, p.voiceO);
    }
    if (p.chart.notes.empty()) { p.error = "chart has no notes"; return p; }
    p.ok = true;
    return p;
}

Prepared Library::reloadAudio(const SongEntry& e) {
    Prepared p;
    if (e.builtin && e.def) {
        const auto a = builtinAudio(*e.def);
        p.inst = a.inst;
        p.voiceP = a.vp;
        p.voiceO = a.vo;
    } else {
        std::lock_guard<std::mutex> lk(customMtx_);
        customInstKey_ = customCacheKey_ = e.id;
        customInst_ = streamOrNull(e.inst);
        customVoices_ = streamOrNull(e.voices);
        customVp_ = streamOrNull(e.voicesP);
        customVo_ = streamOrNull(e.voicesO);
        p.inst = customInst_;
        if (customVp_ || customVo_) {
            p.voiceP = customVp_;
            p.voiceO = customVo_;
        } else {
            p.voiceP = customVoices_;
        }
    }
    p.ok = p.inst != nullptr;
    if (!p.ok) p.error = "cannot decode " + e.inst;
    return p;
}

PcmPtr Library::preview(const SongEntry& e) {
    if (e.builtin && e.def) return builtinAudio(*e.def).inst;
    std::lock_guard<std::mutex> lk(customMtx_);
    if (customInstKey_ != e.id) {
        // Only the instrumental is needed for a preview; prepare() loads voices.
        // The menu plays it from 30% in: decoded from there first.
        customInstKey_ = e.id;
        customCacheKey_.clear();
        customInst_ = streamOrNull(e.inst, 0.3);
    }
    return customInst_;
}

PcmPtr Library::menuMusic(double* loopSec) {
    std::lock_guard<std::mutex> lk(menuMtx_);
    if (!menu_) {
        auto comp = compose(menuTheme(), true);
        auto p = std::make_shared<Pcm>();
        p->channels = 2;
        p->data = std::move(comp->inst);
        menu_ = p;
        menuLoop_ = comp->loopSec;
    }
    if (loopSec) *loopSec = menuLoop_;
    return menu_;
}

}  // namespace gh
