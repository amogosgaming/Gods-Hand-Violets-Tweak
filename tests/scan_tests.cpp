// God's Hand of Principality and Demise. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// Finding songs in every mod layout (song_scan.cpp), PulseForge charts, audio
// lengths and assets shared between mods.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "chart.hpp"
#include "chart_binary.hpp"
#include "chart_iutk.hpp"
#include "chart_stream.hpp"
#include "common.hpp"
#include "flp_import.hpp"
#include "scene.hpp"
#include "session.hpp"
#include "song_scan.hpp"

using namespace gh;
namespace fs = std::filesystem;

extern int g_fail, g_pass;
#define SCHECK(cond)                                                             \
    do {                                                                         \
        if (cond) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

namespace {

std::string str(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A silent 8 kHz mono 8-bit WAV of the given length.
void putWav(const fs::path& p, double seconds) {
    const uint32_t rate = 8000, data = uint32_t(seconds * rate);
    std::string w = "RIFF";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w += char((v >> (8 * i)) & 0xFF); };
    auto u16 = [&](uint16_t v) { w += char(v & 0xFF); w += char(v >> 8); };
    u32(36 + data);
    w += "WAVEfmt ";
    u32(16);
    u16(1);      // PCM
    u16(1);      // mono
    u32(rate);
    u32(rate);   // bytes per second
    u16(1);
    u16(8);
    w += "data";
    u32(data);
    w.append(data, char(0x80));
    put(p, w);
}

std::string legacyChart(const std::string& song, int notes) {
    std::string s = "{\"song\":{\"song\":\"" + song + "\",\"bpm\":120,\"speed\":2,\"notes\":[{\"mustHitSection\":true,\"sectionNotes\":[";
    for (int i = 0; i < notes; ++i) s += (i ? "," : "") + std::string("[") + std::to_string(500 * i) + "," + std::to_string(i % 4) + ",0]";
    return s + "]}]}}";
}

const SongEntry* byTitle(const SongScan& s, const std::string& title, int nth = 0) {
    for (const SongEntry& e : s.songs)
        if (equalsI(e.title, title) && nth-- == 0) return &e;
    return nullptr;
}

int countTitle(const SongScan& s, const std::string& title) {
    int n = 0;
    for (const SongEntry& e : s.songs) n += equalsI(e.title, title);
    return n;
}

bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

const char* kPulseForgeEditor = R"({"format":"pulseforge-editor-chart","version":1,
 "metadata":{"title":"Forge Song","difficulty":"hard","stage":"street","player":"bf","opponent":"pico","girlfriend":"gf"},
 "keyCount":6,"scrollSpeed":2.5,
 "tempos":[{"id":1,"time":0,"bpm":150,"numerator":4,"denominator":4},{"id":2,"time":4000,"bpm":200}],
 "notes":[{"id":2,"time":0,"duration":0,"lane":5,"owner":"player","kind":"normal","payloadId":0},
          {"id":3,"time":100,"duration":400,"lane":1,"owner":"opponent","kind":"normal"},
          {"id":4,"time":200,"duration":0,"lane":2,"owner":"girlfriend","kind":"normal"},
          {"id":5,"time":300,"duration":0,"lane":3,"owner":"player","kind":"Hurt Note"},
          {"id":6,"time":300.4,"duration":0,"lane":3,"owner":"player","kind":"Hurt Note"},
          {"id":7,"time":5000,"duration":5,"lane":0,"owner":"player"}],
 "events":[{"time":1000,"name":"Hey!","value1":"bf","value2":"0.6"},[2000,[["Change Scroll Speed","1.5","0"]]]]})";

void testPulseForgeCharts() {
    Chart c;
    std::string err;
    SCHECK(parseChart(kPulseForgeEditor, "Hard", c, err));
    SCHECK(c.title == "Forge Song");
    SCHECK(c.keys == 6);
    SCHECK(std::fabs(c.speed - 2.5) < 1e-9);
    SCHECK(c.player == "bf" && c.opponent == "pico" && c.gf == "gf" && c.stage == "street");
    SCHECK(c.tempo.size() == 2 && std::fabs(c.bpmAt(4500) - 200) < 1e-9);
    SCHECK(c.notes.size() == 5);  // the two hurt notes 0.4 ms apart are one stacked note
    SCHECK(c.rawNoteCount == 6);
    SCHECK(c.playerNotes == 4 && c.opponentNotes == 2);
    if (c.notes.size() == 5) {
        SCHECK(c.notes[0].side == 1 && c.notes[0].lane == 5);
        SCHECK(c.notes[1].side == 0 && c.notes[1].lane == 1 && std::fabs(c.notes[1].length - 400) < 1e-9);
        SCHECK(c.notes[2].side == 0 && (c.notes[2].flags & kFlagGF));
        SCHECK(c.notes[3].kind == kMine && c.notes[3].stack == 2);
        SCHECK(c.notes[4].length == 0);  // shorter than a step: a tap
    }
    SCHECK(c.events.size() == 2 && c.events[0].name == "Hey!" && c.events[0].v2 == "0.6" &&
           c.events[1].name == "Change Scroll Speed" && c.events[1].v1 == "1.5");

    // Extra keys off: 4 lanes, as for every other chart.
    Chart four;
    SCHECK(parseChart(kPulseForgeEditor, "Hard", four, err, {}, false));
    SCHECK(four.keys == 4 && !four.notes.empty() && four.notes[0].lane == 1);

    // The exported format keeps the scene in "song".
    const char* exported = R"({"format":"pulseforge-chart","version":1,
      "song":{"title":"Fantrack","difficulty":"expert","keyCount":4,"scrollSpeed":1},
      "audio":{"instrumental":"Inst.mp3","vocals":[]},
      "tempos":[{"time":0,"bpm":193.9655,"numerator":4,"denominator":4}],
      "notes":[{"time":725.3,"duration":0,"lane":2,"owner":"player","kind":"normal"},
               {"time":826.6,"duration":0,"lane":1,"owner":"opponent","kind":"normal"}],"events":[]})";
    Chart e;
    SCHECK(parseChart(exported, "Expert", e, err));
    SCHECK(e.title == "Fantrack" && e.keys == 4 && e.notes.size() == 2 && e.playerNotes == 1);

    // Big files are streamed: the same chart read either way.
    const fs::path f = fs::temp_directory_path() / "gh_pulseforge_stream.pfchart.json";
    put(f, kPulseForgeEditor);
    Chart streamed;
    SCHECK(loadChartFile(str(f), "Hard", streamed, err, {}, 1));
    SCHECK(streamed.notes.size() == c.notes.size() && streamed.rawNoteCount == 6 && streamed.keys == 6);
    SCHECK(streamed.playerNotes == 4 && streamed.opponentNotes == 2 && streamed.events.size() == 2);
    if (streamed.notes.size() == 5) {
        SCHECK(streamed.notes[2].side == 0 && (streamed.notes[2].flags & kFlagGF));
        SCHECK(streamed.notes[3].kind == kMine && streamed.notes[3].stack == 2);
    }
    const ChartPeek pk = peekChart(str(f));
    SCHECK(pk.pulseForge && pk.isChart && pk.song == "Forge Song" && pk.difficulty == "hard");
    fs::remove(f);
}

void testAudioSeconds() {
    const fs::path dir = fs::temp_directory_path() / "gh_audio_len";
    fs::remove_all(dir);
    putWav(dir / "a.wav", 3.5);
    SCHECK(std::fabs(audioSeconds(str(dir / "a.wav")) - 3.5) < 0.01);

    // Ogg Vorbis: the rate from the identification header, the length from the last page.
    std::string ogg = "OggS";
    ogg.append(1, '\0');
    ogg.append(1, '\2');
    ogg.append(8, '\0');
    ogg.append(12, '\0');
    ogg += char(1);
    ogg += char(30);
    ogg += "\x01vorbis";
    ogg.append(4, '\0');
    ogg += char(2);
    const uint32_t rate = 44100;
    for (int i = 0; i < 4; ++i) ogg += char((rate >> (8 * i)) & 0xFF);
    ogg.append(2000, 'x');
    ogg += "OggS";
    ogg.append(1, '\0');
    ogg.append(1, '\4');
    const uint64_t granule = uint64_t(rate) * 95;
    for (int i = 0; i < 8; ++i) ogg += char((granule >> (8 * i)) & 0xFF);
    ogg.append(16, '\0');
    put(dir / "b.ogg", ogg);
    SCHECK(std::fabs(audioSeconds(str(dir / "b.ogg")) - 95) < 0.01);

    // MP3: 128 kbps frames after an ID3 tag.
    std::string mp3 = "ID3";
    mp3 += char(4);
    mp3.append(2, '\0');
    mp3 += std::string("\0\0\0\x0A", 4);  // tag of 10 bytes
    mp3.append(10, '\0');
    mp3 += "\xFF\xFB\x90\x00";
    mp3.append(1600000 - mp3.size(), '\0');
    put(dir / "c.mp3", mp3);
    const double secs = audioSeconds(str(dir / "c.mp3"));
    SCHECK(secs > 99 && secs < 101);
    fs::remove_all(dir);
}

void testModLayouts() {
    const fs::path base = fs::temp_directory_path() / "gh_scan_layouts";
    fs::remove_all(base);
    const fs::path mods = base / "mods";

    // A Psych Engine build: charts in assets/preload/data/<song>/ and
    // assets/shared/data/<song>/, audio in assets/songs/<song>/.
    const fs::path game = mods / "packA" / "Game Build";
    const fs::path assets = game / "assets";
    put(assets / "songs" / "foo" / "Inst.ogg", "x");
    put(assets / "songs" / "foo" / "Voices.ogg", "x");
    put(assets / "preload" / "data" / "foo" / "foo.json", legacyChart("Foo", 8));
    put(assets / "preload" / "data" / "foo" / "foo-hard.json", legacyChart("Foo", 12));
    put(assets / "preload" / "data" / "foo" / "dialogue.json", R"({"dialogue":[{"portrait":"bf","text":"hi","speed":0.05}]})");
    put(assets / "songs" / "qux" / "Inst.ogg", "x");
    put(assets / "shared" / "data" / "qux" / "qux.json", legacyChart("Qux", 4));
    put(assets / "shared" / "images" / "stageback.png", "png");
    put(assets / "shared" / "data" / "characters" / "bf.json", R"({"animations":[],"image":"characters/BOYFRIEND","scale":1})");
    // data/charts/<song>/ and a chart straight in data/
    put(assets / "songs" / "bar" / "Inst.ogg", "x");
    put(assets / "data" / "charts" / "bar" / "bar.json", legacyChart("Bar", 5));
    put(assets / "data" / "loose-song.json", legacyChart("Loose Song", 6));
    // The build's mods/ folder holds charts of its own and a mod folder.
    put(game / "mods" / "data" / "global" / "global.json", legacyChart("Global", 3));
    put(game / "mods" / "songs" / "global" / "Inst.ogg", "x");
    put(game / "mods" / "MyMod" / "data" / "baz" / "baz.json", legacyChart("Baz", 7));
    put(game / "mods" / "MyMod" / "songs" / "baz" / "Inst.ogg", "x");
    put(game / "mods" / "MyMod" / "characters" / "pico.json", R"({"animations":[]})");
    // An exported copy of the build deep inside: the same chart is offered once.
    const fs::path exportAssets = game / "export" / "release" / "windows" / "bin" / "assets";
    put(exportAssets / "data" / "foo" / "foo.json", legacyChart("Foo", 8));
    put(exportAssets / "songs" / "foo" / "Inst.ogg", "x");
    // ...and a second, different version of a song: both are offered, with their folder.
    put(exportAssets / "data" / "qux" / "qux.json", legacyChart("Qux", 9));
    put(exportAssets / "songs" / "qux" / "Inst.ogg", "x");

    // A PulseForge chart with its audio; editor projects and autosaves.
    const fs::path packB = mods / "packB";
    put(packB / "chart.json", R"({"format":"pulseforge-chart","version":1,"song":{"title":"Fantrack","difficulty":"expert","keyCount":4},
        "tempos":[{"time":0,"bpm":120}],"notes":[{"time":0,"duration":0,"lane":1,"owner":"player","kind":"normal"}],"events":[]})");
    put(packB / "Inst.mp3", "x");
    put(packB / "autosaves" / "victim-easy.autosave.pfchart.json", kPulseForgeEditor);
    put(packB / "autosaves" / "untitled.autosave.pfchart.json",
        R"({"format":"pulseforge-editor-chart","version":1,"metadata":{"title":"Empty"},"keyCount":4,"notes":[],"events":[]})");
    put(packB / "musical-conversions" / "template.pfm.json", R"({"format":"pulseforge-pfm-source-v1","notes":[],"scrollSpeed":1})");
    put(packB / "images" / "icons" / "icon-pico.png", "png");
    // A recovered chart with only events: nothing to play, no song.
    put(packB / "charts" / "_unknown" / "unknown-1.json",
        R"({"song":{"song":"Mystery","bpm":150,"events":[[100,[["Hey!","",""]]]],"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})");
    // A lone section recovered from a bigger chart plays as a chart.
    put(packB / "charts" / "_fragments" / "frag-1.json",
        R"({"lengthInSteps":16,"mustHitSection":false,"sectionNotes":[[100,0,0],[200,5,0]]})");
    // Title screen and freeplay files in data/ are no songs.
    put(assets / "data" / "title" / "offsets.json", R"({"bpm":102,"song":"freakyMenu","x":0})");

    // Audio-only folders without charts are not auto-charted.
    const fs::path packC = mods / "packC" / "loose songs";
    putWav(packC / "Long Song_Inst.wav", 31);
    putWav(packC / "Long Song_Voices.wav", 31);
    putWav(packC / "whoosh.wav", 2);
    putWav(packC / "Inst.wav", 40);

    SongScan s;
    scanModsDir(str(mods), s);
    if (std::getenv("GH_SCAN_DEBUG"))
        for (const SongEntry& e : s.songs) {
            std::printf("%s | %s | %s | inst %s\n", e.id.c_str(), e.title.c_str(), e.subtitle.c_str(), e.inst.c_str());
            for (size_t i = 0; i < e.diffs.size(); ++i) std::printf("   [%s] %s\n", e.diffs[i].c_str(), e.chartPaths[i].c_str());
        }

    const SongEntry* foo = byTitle(s, "Foo");
    SCHECK(foo && foo->diffs.size() == 2);
    SCHECK(countTitle(s, "Foo") == 1);
    if (foo) {
        SCHECK(foo->chartPaths.size() == 2 && foo->chartPaths[0].find("preload") != std::string::npos);
        SCHECK(endsWith(foo->inst, "assets/songs/foo/Inst.ogg") && endsWith(foo->voices, "Voices.ogg"));
        SCHECK(foo->pack == "packA");
        for (const std::string& d : foo->diffs) SCHECK(!equalsI(d, "Dialogue"));
        bool hasShared = false;
        for (const std::string& r : foo->assetRoots) hasShared |= endsWith(r, "assets/shared/");
        SCHECK(hasShared);
    }
    SCHECK(countTitle(s, "Qux") == 2);
    const SongEntry* qux = byTitle(s, "Qux");
    SCHECK(qux && endsWith(qux->inst, "Inst.ogg"));
    if (const SongEntry* q2 = byTitle(s, "Qux", 1)) SCHECK(q2->subtitle != qux->subtitle);
    SCHECK(!byTitle(s, "Characters"));
    const SongEntry* bar = byTitle(s, "Bar");
    SCHECK(bar && endsWith(bar->inst, "assets/songs/bar/Inst.ogg"));
    const SongEntry* loose = byTitle(s, "Loose Song");
    SCHECK(loose && loose->chartPaths.size() == 1 && endsWith(loose->modRoot, "assets/"));
    SCHECK(byTitle(s, "Global") && byTitle(s, "Baz"));
    if (const SongEntry* baz = byTitle(s, "Baz")) SCHECK(endsWith(baz->inst, "MyMod/songs/baz/Inst.ogg"));

    const SongEntry* fan = byTitle(s, "Fantrack");
    SCHECK(fan && fan->diffs.size() == 1 && fan->diffs[0] == "Expert" && endsWith(fan->inst, "packB/Inst.mp3"));
    const SongEntry* victim = byTitle(s, "Forge Song");
    SCHECK(victim && victim->diffs.size() == 1 && victim->diffs[0] == "Hard (autosave)");  // the chart's own difficulty
    SCHECK(!byTitle(s, "Empty") && !byTitle(s, "Template"));
    SCHECK(!byTitle(s, "Mystery") && !byTitle(s, "Title") && !byTitle(s, "Offsets"));
    const SongEntry* frag = byTitle(s, "Frag");
    SCHECK(frag && frag->chartPaths.size() == 1);
    if (frag && !frag->chartPaths.empty()) {
        Chart fc;
        std::string ferr;
        SCHECK(loadChartFile(frag->chartPaths[0], "Normal", fc, ferr));
        SCHECK(fc.notes.size() == 2 && fc.opponentNotes == 1 && fc.playerNotes == 1);
    }

    SCHECK(!byTitle(s, "Long Song"));
    SCHECK(!byTitle(s, "Loose Songs"));
    SCHECK(!byTitle(s, "Whoosh"));

    std::vector<std::string> ids;
    for (const SongEntry& e : s.songs) ids.push_back(e.id);
    std::sort(ids.begin(), ids.end());
    SCHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Asset folders: the song's own mod first, then the other mods.
    SCHECK(s.packRoots.count("packA") && s.packRoots.count("packB"));
    if (fan) {
        const std::vector<std::string> shared = sharedRootsFor(s, *fan);
        SCHECK(!shared.empty() && endsWith(shared.front(), "packB/"));
        bool packA = false;
        for (const std::string& r : shared) packA |= endsWith(r, "MyMod/") || endsWith(r, "assets/shared/");
        SCHECK(packA);
    }

    // The song's own folders first; another mod's files only through find().
    scene::Files files;
    files.setRoots({str(packB) + "/"});
    files.setSharedRoots(fan ? sharedRootsFor(s, *fan) : std::vector<std::string>{});
    SCHECK(endsWith(files.find("images/icons/icon-pico.png"), "packB/images/icons/icon-pico.png"));
    SCHECK(endsWith(files.find("characters/pico.json"), "MyMod/characters/pico.json"));
    SCHECK(endsWith(files.find("images/stageback.png"), "assets/shared/images/stageback.png"));
    SCHECK(files.find("characters/nobody.json").empty());
    SCHECK(files.list("characters", ".json").empty());  // scripts of other mods never run

    // A disabled mod is left out.
    put(mods / "modsList.txt", "packB|0\n");
    SongScan off;
    scanModsDir(str(mods), off);
    SCHECK(!byTitle(off, "Fantrack") && byTitle(off, "Foo"));
    fs::remove_all(base);
}

// A small FL Studio project: an FPC channel with a chart in pattern 1
// ("hard"), one note in pattern 2 ("easy"), a melody in pattern 3, and a
// second channel whose notes are not the chart's.
std::string makeFlp() {
    std::string ev;
    auto byte = [&](uint8_t id, uint8_t v) { ev += char(id); ev += char(v); };
    auto word = [&](uint8_t id, uint16_t v) { ev += char(id); ev += char(v & 255); ev += char(v >> 8); };
    auto dword = [&](uint8_t id, uint32_t v) {
        ev += char(id);
        for (int i = 0; i < 4; ++i) ev += char((v >> (8 * i)) & 255);
    };
    auto array = [&](uint8_t id, const std::string& data) {
        ev += char(id);
        size_t n = data.size();
        do {
            uint8_t c = uint8_t(n & 0x7F);
            n >>= 7;
            if (n) c |= 0x80;
            ev += char(c);
        } while (n);
        ev += data;
    };
    auto utf16 = [](const std::string& a) {
        std::string w;
        for (char c : a) { w += c; w += char(0); }
        return w + std::string(2, '\0');
    };
    struct N { uint32_t t; uint16_t ch; uint32_t dur, pitch; uint8_t vel; };
    auto notes = [](const std::vector<N>& v) {
        std::string d;
        for (const N& n : v) {
            std::string r(24, '\0');
            for (int i = 0; i < 4; ++i) r[size_t(i)] = char((n.t >> (8 * i)) & 255);
            r[6] = char(n.ch & 255);
            for (int i = 0; i < 4; ++i) r[size_t(8 + i)] = char((n.dur >> (8 * i)) & 255);
            for (int i = 0; i < 4; ++i) r[size_t(12 + i)] = char((n.pitch >> (8 * i)) & 255);
            r[21] = char(n.vel);
            d += r;
        }
        return d;
    };
    array(0xC7, "20.8.4.2576");
    dword(0x9C, 150000);  // 150 BPM
    array(0xC2, utf16("Forge Test"));
    word(0x40, 0);
    array(0xC9, utf16("FPC"));
    word(0x40, 1);
    array(0xC9, utf16("3x Osc"));
    byte(0x1C, 3);
    word(0x41, 1);
    array(0xC1, utf16("hard"));
    word(0x41, 2);
    array(0xC1, utf16("easy"));
    word(0x41, 1);
    array(0xE0, notes({{0, 0, 24, 54, 100},     // camera on the opponent
                       {0, 0, 24, 60, 100},     // opponent left
                       {96, 0, 24, 61, 100},    // opponent down, one beat in
                       {384, 0, 24, 53, 100},   // section 1: camera on the player
                       {384, 0, 96, 48, 100},   // player left, held a beat
                       {480, 0, 48, 51, 50},    // player right, soft: a hold
                       {768, 0, 24, 58, 100},   // section 2: the player's alt animation
                       {768, 0, 24, 49, 100},   // player down
                       {100, 1, 24, 60, 100}}));  // another channel: not the chart
    word(0x41, 2);
    array(0xE0, notes({{0, 0, 24, 50, 100}}));
    word(0x41, 3);
    array(0xE0, notes({{0, 0, 96, 65, 100}, {96, 0, 96, 67, 100}, {192, 0, 96, 69, 100}, {288, 0, 96, 60, 100},
                       {384, 0, 96, 72, 100}, {480, 0, 96, 71, 100}, {576, 0, 96, 64, 100}}));
    std::string f = "FLhd";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f += char((v >> (8 * i)) & 255); };
    u32(6);
    f += std::string("\0\0\x02\0", 4);
    f += char(96);
    f += char(0);
    f += "FLdt";
    u32(uint32_t(ev.size()));
    return f + ev;
}

void testFlStudio() {
    FlpProject p;
    std::string err;
    SCHECK(readFlp(makeFlp(), p, err));
    SCHECK(p.ppqn == 96 && std::fabs(p.bpm - 150) < 1e-9 && p.title == "Forge Test" && p.chartChannel == 0);
    SCHECK(p.patterns.size() == 3 && p.patterns[0].name == "hard" && p.patterns[1].name == "easy");
    SCHECK(flpChartPatterns(p, true) == std::vector<int>({1, 2}));         // the melody is no chart
    SCHECK(flpChartPatterns(p, false) == std::vector<int>({1, 2, 3}));
    SCHECK(flpPatternLabel(p, 1) == "Hard" && flpPatternLabel(p, 3) == "Pattern 3");

    Chart c;
    SCHECK(flpToChart(p, 1, c, err));
    SCHECK(c.keys == 4 && c.notes.size() == 5 && c.playerNotes == 3 && c.opponentNotes == 2);
    if (c.notes.size() == 5) {
        // 96 ticks per beat at 150 BPM: 400 ms a beat.
        SCHECK(c.notes[0].side == 0 && c.notes[0].lane == 0 && std::fabs(c.notes[0].time) < 1e-6 && c.notes[0].length == 0);
        SCHECK(c.notes[1].side == 0 && c.notes[1].lane == 1 && std::fabs(c.notes[1].time - 400) < 1e-6);
        SCHECK(c.notes[2].side == 1 && c.notes[2].lane == 0 && std::fabs(c.notes[2].time - 1600) < 1e-6 &&
               std::fabs(c.notes[2].length - 300) < 1e-6);  // (96 - 24) ticks
        SCHECK(c.notes[3].side == 1 && c.notes[3].lane == 3 && std::fabs(c.notes[3].length - 100) < 1e-6);  // soft: (48 - 24) ticks
        SCHECK(c.notes[4].side == 1 && c.notes[4].lane == 1 && (c.notes[4].flags & kFlagAlt) && std::fabs(c.notes[4].time - 3200) < 1e-6);
    }
    SCHECK(c.sections.size() == 3 && !c.sections[0].mustHit && c.sections[1].mustHit && c.sections[2].mustHit);
    SCHECK(c.focusAt(100) == 0 && c.focusAt(2000) == 1);
    SCHECK(std::fabs(c.bpmAt(0) - 150) < 1e-9 && c.title == "Forge Test");

    std::string file;
    int pat = 0;
    SCHECK(flpChartPath("a b/song.flp#2", file, pat) && file == "a b/song.flp" && pat == 2);
    SCHECK(flpChartPath("song.FSC", file, pat) && pat == -1);
    SCHECK(!flpChartPath("song.json", file, pat));

    // Through the chart loader and the song scan.
    const fs::path base = fs::temp_directory_path() / "gh_flp_scan";
    fs::remove_all(base);
    put(base / "mods" / "flpack" / "Forge Test.flp", makeFlp());
    Chart loaded;
    SCHECK(loadChartFile(str(base / "mods" / "flpack" / "Forge Test.flp") + "#2", "Easy", loaded, err) && loaded.notes.size() == 1);
    SongScan s;
    scanModsDir(str(base / "mods"), s);
    const SongEntry* e = byTitle(s, "Forge Test");
    SCHECK(e && e->diffs.size() == 2 && e->diffs[0] == "Easy (FL Studio)" && e->diffs[1] == "Hard (FL Studio)");
    fs::remove_all(base);

    // Multikey SNIFF conversion with custom Opponent & Player MIDI ranges.
    SCHECK(midiNoteName(48) == "C3" && midiNoteName(60) == "C4" && midiNoteName(61) == "C#4");
    FlpProject mk;
    mk.ppqn = 96;
    mk.bpm = 120.0;
    mk.title = "MultiKey FLP";
    mk.chartChannel = 0;
    FlpPattern pat6;
    pat6.number = 1;
    pat6.name = "6k";
    for (int i = 0; i < 6; ++i) pat6.notes.push_back({uint32_t(i * 48), 24, uint32_t(48 + i), 0, 100, 0});  // Player 6K: 48..53
    for (int i = 0; i < 9; ++i) pat6.notes.push_back({uint32_t(384 + i * 48), 24, uint32_t(60 + i), 0, 100, 0});  // Opp 9K: 60..68
    mk.patterns.push_back(pat6);

    FlpConvertOptions opt;
    opt.playerStartPitch = 48;
    opt.playerKeys = 6;
    opt.oppStartPitch = 60;
    opt.oppKeys = 9;
    opt.useMarkers = true;
    Chart mkc;
    SCHECK(flpToChart(mk, 1, opt, mkc, err));
    SCHECK(mkc.keys == 9 && mkc.keysOf(0) == 9 && mkc.keysOf(1) == 6);
    SCHECK(mkc.playerNotes == 6 && mkc.opponentNotes == 9 && mkc.notes.size() == 15);
    if (mkc.notes.size() == 15) {
        SCHECK(mkc.notes[5].side == 1 && mkc.notes[5].lane == 5);
        SCHECK(mkc.notes[14].side == 0 && mkc.notes[14].lane == 8);
    }
}

}  // namespace

// gh_tests --flp a.flp ...: what an FL Studio file holds, as SNIFF sees it.
int printFlp(int argc, char** argv, int first) {
    for (int i = first; i < argc; ++i) {
        FlpProject p;
        std::string err;
        if (!readFlpFile(argv[i], p, err)) {
            std::printf("%s: %s\n", argv[i], err.c_str());
            continue;
        }
        std::printf("%s: ppqn %d, bpm %.3f, title '%s', chart channel %d, %zu patterns\n", argv[i], p.ppqn, p.bpm,
                    p.title.c_str(), p.chartChannel, p.patterns.size());
        for (const FlpPattern& pat : p.patterns) {
            size_t chart = 0, onChannel = 0;
            for (const FlpNote& n : pat.notes) {
                onChannel += n.channel == p.chartChannel;
                chart += n.channel == p.chartChannel && ((n.pitch >= 48 && n.pitch <= 51) || (n.pitch >= 60 && n.pitch <= 63));
            }
            std::printf("  pattern %d '%s': %zu notes, %zu on the chart channel, %zu in the chart lanes\n", pat.number,
                        pat.name.c_str(), pat.notes.size(), onChannel, chart);
        }
        Chart c;
        if (flpToChart(p, -1, c, err)) std::printf("  chart: %zu notes, %.1f s\n", c.notes.size(), c.lengthMs / 1000.0);
        else std::printf("  no chart: %s\n", err.c_str());
    }
    return 0;
}

// Binary charts (.ghc) in a mod: one kept alone, without its JSON (a chart
// too big to keep as JSON), is a chart like any other - not an audio-only
// song for AutoChart; one beside its JSON is offered once, as the .ghc.
void testBinaryChartsInMods() {
    const fs::path base = fs::temp_directory_path() / "gh_scan_ghc";
    fs::remove_all(base);
    const fs::path mod = base / "mods" / "ghcpack";
    put(mod / "songs" / "alone" / "Inst.ogg", "x");
    put(mod / "songs" / "paired" / "Inst.ogg", "x");
    put(base / "alone.json", legacyChart("Alone", 8));
    put(mod / "data" / "paired" / "paired.json", legacyChart("Paired", 8));
    put(mod / "data" / "paired" / "paired-hard.json", legacyChart("Paired", 12));
    std::string err;
    fs::create_directories(mod / "data" / "alone");
    SCHECK(convertChartToBinary(str(base / "alone.json"), str(mod / "data" / "alone" / "alone.ghc"), err));
    SCHECK(convertChartToBinary(str(mod / "data" / "paired" / "paired.json"), str(mod / "data" / "paired" / "paired.ghc"), err));
    SongScan s;
    scanModsDir(str(base / "mods"), s);
    const SongEntry* alone = nullptr;
    const SongEntry* paired = nullptr;
    for (const SongEntry& e : s.songs) {
        if (e.songKey == "alone") alone = &e;
        if (e.songKey == "paired") paired = &e;
    }
    SCHECK(alone && alone->diffs.size() == 1 && alone->diffs[0] == "Normal" &&
           endsWithI(alone->chartPaths[0], ".ghc"));
    SCHECK(paired && paired->diffs.size() == 2 && paired->diffs[0] == "Normal" && endsWithI(paired->chartPaths[0], ".ghc") &&
           paired->diffs[1] == "Hard" && endsWithI(paired->chartPaths[1], "paired-hard.json"));
    if (alone && !alone->chartPaths.empty()) {
        Chart c;
        SCHECK(loadChartFile(alone->chartPaths[0], "Normal", c, err) && c.title == "Alone" && !c.notes.empty());
    }
    fs::remove_all(base);
}

void testIutkCharts() {
    const fs::path base = fs::temp_directory_path() / "gh_test_iutk";
    fs::remove_all(base);
    std::string err;

    // 1. Round-trip across V2, V3, V4 (separate metadata.json), V5 (embedded), and None (defaults).
    {
        IutkMetadata meta;
        meta.title = "Spam Galaxy";
        meta.difficulty = "Hard";
        meta.player = "bf-dark";
        meta.opponent = "pico";
        meta.gf = "gf-car";
        meta.stage = "philly";
        meta.bpm = 175.0;
        meta.speed = 3.4;
        meta.needsVoices = true;
        meta.keys = 4;
        meta.sections.push_back({0.0, false, false, false, 4.0, 175.0});
        meta.sections.push_back({1371.428, true, true, false, 4.0, 200.0});

        std::vector<psych::ChartEvent> events;
        events.push_back({500.0, "Hey!", "bf", "0.6"});
        events.push_back({1500.0, "Change Scroll Speed", "1.25", "0.5"});

        std::vector<IutkInputNote> notes;
        notes.push_back({-50.0, 0, 0, 1});
        notes.push_back({0.0, 0, 1, 3});
        notes.push_back({0.0, 1, 2, 2});
        notes.push_back({250.4, 1, 3, 1});
        notes.push_back({1000.0, 0, 3, 1});
        notes.push_back({1000.0, 1, 0, 5});

        // V4 with separate metadata.json + events.json
        const fs::path d4 = base / "v4_sep";
        fs::create_directories(d4);
        IutkWriteOptions opt4;
        opt4.version = IutkVersion::V4;
        opt4.metaMode = IutkMetaMode::SeparateFile;
        opt4.rleMode = IutkRleMode::Optimal;
        opt4.saveEventsJson = true;
        IutkWriteResult res4;
        SCHECK(writeIutkChartNotes(str(d4 / "Chart.bin"), notes, meta, events, opt4, err, &res4));
        SCHECK(isIutkChart(str(d4 / "Chart.bin")));
        SCHECK(fs::exists(d4 / "metadata.json") && fs::exists(d4 / "events.json"));
        SCHECK(res4.cells == 6 && res4.totalNotes == 13 && res4.metaBytes > 0 && res4.eventsBytes > 0);

        IutkInfo info4;
        SCHECK(inspectIutkChart(str(d4 / "Chart.bin"), info4));
        SCHECK(info4.valid && info4.version == 4 && !info4.hasEmbeddedMeta && info4.hasSiblingMeta);
        SCHECK(info4.cellCount == 6 && info4.totalNotes == 13);
        SCHECK(info4.meta.title == "Spam Galaxy" && info4.meta.opponent == "pico" && info4.meta.player == "bf-dark");

        Chart c4;
        SCHECK(loadChartFile(str(d4 / "Chart.bin"), "Normal", c4, err));
        SCHECK(c4.title == "Spam Galaxy" && c4.player == "bf-dark" && c4.opponent == "pico" && c4.gf == "gf-car" && c4.stage == "philly");
        SCHECK(std::fabs(c4.speed - 3.4) < 1e-6 && std::fabs(c4.bpmAt(0) - 175.0) < 1e-6 && std::fabs(c4.bpmAt(1500.0) - 200.0) < 1e-6);
        SCHECK(c4.notes.size() == 6 && c4.opponentNotes == 5 && c4.playerNotes == 8 && c4.rawNoteCount == 13);
        SCHECK(c4.events.size() == 2 && c4.events[0].name == "Hey!" && c4.events[1].name == "Change Scroll Speed");
        SCHECK(std::fabs(c4.notes[0].time - (-50.0)) < 1e-6 && c4.notes[0].side == 0 && c4.notes[0].lane == 0);
        SCHECK(c4.notes[5].side == 1 && c4.notes[5].lane == 0 && c4.notes[5].stack == 5);

        // V5 with embedded metadata (no sibling metadata.json)
        const fs::path d5 = base / "v5_emb";
        fs::create_directories(d5);
        IutkWriteOptions opt5 = opt4;
        opt5.metaMode = IutkMetaMode::Embedded;
        opt5.compressEmbeddedMeta = true;
        IutkWriteResult res5;
        SCHECK(writeIutkChartNotes(str(d5 / "Chart.bin"), notes, meta, events, opt5, err, &res5));
        SCHECK(!fs::exists(d5 / "metadata.json") && fs::exists(d5 / "events.json"));
        IutkInfo info5;
        SCHECK(inspectIutkChart(str(d5 / "Chart.bin"), info5));
        SCHECK(info5.valid && info5.version == 5 && info5.hasEmbeddedMeta && !info5.hasSiblingMeta);
        Chart c5;
        SCHECK(loadChartFile(str(d5 / "Chart.bin"), "Hard", c5, err));
        SCHECK(c5.title == "Spam Galaxy" && c5.opponent == "pico" && std::fabs(c5.speed - 3.4) < 1e-6);
        SCHECK(c5.notes.size() == 6 && c5.playerNotes == 8 && c5.opponentNotes == 5 && c5.events.size() == 2);

        // V3 (convert-chart.c compatible) + V2 (flat) with NO metadata -> defaults (speed 7, dad, bf, gf, stage, 120 BPM)
        const fs::path d3 = base / "v3_none";
        fs::create_directories(d3);
        IutkWriteOptions opt3;
        opt3.version = IutkVersion::V3;
        opt3.metaMode = IutkMetaMode::None;
        opt3.saveEventsJson = false;
        SCHECK(writeIutkChartNotes(str(d3 / "Chart.bin"), notes, meta, {}, opt3, err));
        SCHECK(!fs::exists(d3 / "metadata.json") && !fs::exists(d3 / "events.json"));
        Chart c3;
        SCHECK(loadChartFile(str(d3 / "Chart.bin"), "Normal", c3, err));
        SCHECK(std::fabs(c3.speed - 7.0) < 1e-9);
        SCHECK(c3.opponent == "dad" && c3.player == "bf" && c3.gf == "gf" && c3.stage == "stage");
        SCHECK(std::fabs(c3.bpmAt(0) - 120.0) < 1e-9 && c3.keys == 4);
        SCHECK(c3.notes.size() == 6 && c3.playerNotes == 8 && c3.opponentNotes == 5);

        const fs::path d2 = base / "v2_none";
        fs::create_directories(d2);
        IutkWriteOptions opt2 = opt3;
        opt2.version = IutkVersion::V2;
        SCHECK(writeIutkChartNotes(str(d2 / "Chart.bin"), notes, meta, {}, opt2, err));
        Chart c2;
        SCHECK(loadChartFile(str(d2 / "Chart.bin"), "Normal", c2, err));
        SCHECK(std::fabs(c2.speed - 7.0) < 1e-9 && c2.opponent == "dad" && c2.player == "bf" && c2.gf == "gf");
        SCHECK(c2.notes.size() == 6 && c2.playerNotes == 8 && c2.opponentNotes == 5);
    }

    // 2. 64-bit huge note stacks (5 billion notes in one cell) through Chart & Session.
    {
        const fs::path dHuge = base / "huge_stack";
        fs::create_directories(dHuge);
        const uint64_t kFiveBillion = 5000000000ull;
        std::vector<IutkInputNote> notes = {
            {100.0, 0, 0, kFiveBillion},
            {200.0, 1, 2, kFiveBillion},
            {200.0, 1, 2, 12345ull},  // merged into same cell -> 5,000,012,345
        };
        IutkWriteOptions opt;
        opt.version = IutkVersion::V4;
        opt.metaMode = IutkMetaMode::None;
        opt.saveEventsJson = false;
        SCHECK(writeIutkChartNotes(str(dHuge / "Chart.bin"), notes, IutkMetadata{}, {}, opt, err));
        Chart ch;
        SCHECK(loadChartFile(str(dHuge / "Chart.bin"), "Normal", ch, err));
        SCHECK(ch.notes.size() == 2);
        SCHECK(ch.opponentNotes == kFiveBillion);
        SCHECK(ch.playerNotes == kFiveBillion + 12345ull);
        SCHECK(ch.notes[1].stack == kFiveBillion + 12345ull);

        Session ses;
        ses.init(ch, SessionConfig{});
        SCHECK(ses.totalOpponentNotes == kFiveBillion);
        SCHECK(ses.totalPlayerNotes == kFiveBillion + 12345ull);
        SCHECK(!ses.hasMines(0, 0) && !ses.hasMines(1, 2));
        SCHECK(ses.maxSustain(1, 2) == 0.0);
        ses.update(150.0);
        SCHECK(ses.opponentHits == static_cast<long long>(kFiveBillion));
        ses.press(2, 200.0);
        SCHECK(ses.combo == static_cast<long long>(kFiveBillion + 12345ull));
        SCHECK(ses.counts[kDivine] == static_cast<long long>(kFiveBillion + 12345ull));
    }

    // 3. RLE mode optimality check & CRC32 corruption rejection.
    {
        const fs::path dRle = base / "rle_check";
        fs::create_directories(dRle);
        std::vector<IutkInputNote> spam;
        spam.reserve(4000);
        for (int i = 0; i < 1000; ++i) {
            const double t = double(i * 10);
            for (uint8_t l = 0; l < 4; ++l) spam.push_back({t, 1, l, (i % 7 == 0) ? 1000ull : 1ull});
        }
        IutkWriteOptions oOpt, oFast, oOff;
        oOpt.metaMode = oFast.metaMode = oOff.metaMode = IutkMetaMode::None;
        oOpt.saveEventsJson = oFast.saveEventsJson = oOff.saveEventsJson = false;
        oOpt.rleMode = IutkRleMode::Optimal;
        oFast.rleMode = IutkRleMode::Fast;
        oOff.rleMode = IutkRleMode::Off;
        IutkWriteResult rOpt, rFast, rOff;
        SCHECK(writeIutkChartNotes(str(dRle / "opt.bin"), spam, IutkMetadata{}, {}, oOpt, err, &rOpt));
        SCHECK(writeIutkChartNotes(str(dRle / "fast.bin"), spam, IutkMetadata{}, {}, oFast, err, &rFast));
        SCHECK(writeIutkChartNotes(str(dRle / "off.bin"), spam, IutkMetadata{}, {}, oOff, err, &rOff));
        SCHECK(rOpt.binBytes <= rFast.binBytes && rFast.binBytes < rOff.binBytes);
        Chart cOpt, cFast, cOff;
        SCHECK(loadChartFile(str(dRle / "opt.bin"), "Normal", cOpt, err));
        SCHECK(loadChartFile(str(dRle / "fast.bin"), "Normal", cFast, err));
        SCHECK(loadChartFile(str(dRle / "off.bin"), "Normal", cOff, err));
        SCHECK(cOpt.notes.size() == 4000 && cOpt.playerNotes == cFast.playerNotes && cOpt.playerNotes == cOff.playerNotes);

        // Flip a byte near the end -> CRC32 mismatch must reject the file!
        std::string bytes;
        SCHECK(readTextFile(str(dRle / "opt.bin"), bytes) && bytes.size() > 20);
        bytes[bytes.size() - 2] ^= 0x55;
        put(dRle / "corrupt.bin", bytes);
        Chart bad;
        SCHECK(!loadChartFile(str(dRle / "corrupt.bin"), "Normal", bad, err));
    }

    // 4. Mod directory layout: [mod]/data/[song]/Chart.bin + metadata.json + events.json
    //    and a second song with ONLY Chart.bin (defaults applied).
    {
        const fs::path mods = base / "mods";
        const fs::path mod = mods / "iutkpack";
        put(mod / "songs" / "meta-song" / "Inst.ogg", "x");
        put(mod / "songs" / "bare-song" / "Inst.ogg", "x");

        // Convert a Psych JSON song into [mod]/data/meta-song/Chart.bin + metadata.json + events.json
        put(base / "src_meta.json",
            R"({"song":{"song":"Meta Song","bpm":144,"speed":4.2,"player1":"bf","player2":"senpai","gfVersion":"gf","stage":"school",
                "events":[[400,[["Hey!","bf","0.5"]]]],"notes":[{"mustHitSection":true,"sectionNotes":[[0,0,0],[250,1,0],[500,4,0]]}]}})");
        fs::create_directories(mod / "data" / "meta-song");
        IutkWriteOptions cOpt;
        cOpt.version = IutkVersion::V4;
        cOpt.metaMode = IutkMetaMode::SeparateFile;
        cOpt.saveEventsJson = true;
        ConvertStats cStats;
        IutkWriteResult cRes;
        SCHECK(convertChartToIutk(str(base / "src_meta.json"), str(mod / "data" / "meta-song" / "Chart.bin"), cOpt, err, &cStats,
                                  &cRes, "Hard"));
        SCHECK(fs::exists(mod / "data" / "meta-song" / "Chart.bin"));
        SCHECK(fs::exists(mod / "data" / "meta-song" / "metadata.json"));
        SCHECK(fs::exists(mod / "data" / "meta-song" / "events.json"));

        // Bare Chart.bin with NO metadata.json
        fs::create_directories(mod / "data" / "bare-song");
        IutkWriteOptions bOpt;
        bOpt.version = IutkVersion::V4;
        bOpt.metaMode = IutkMetaMode::None;
        bOpt.saveEventsJson = false;
        SCHECK(writeIutkChartNotes(str(mod / "data" / "bare-song" / "Chart.bin"),
                                   {{100.0, 0, 0, 1}, {200.0, 1, 1, 1}}, IutkMetadata{}, {}, bOpt, err));

        SongScan scan;
        scanModsDir(str(mods), scan);
        const SongEntry* ms = byTitle(scan, "Meta Song");
        const SongEntry* bs = byTitle(scan, "Bare Song");
        SCHECK(ms != nullptr && bs != nullptr);
        if (ms) {
            SCHECK(ms->diffs.size() == 1 && ms->diffs[0] == "Hard");
            SCHECK(endsWithI(ms->chartPaths[0], "Chart.bin"));
            SCHECK(ms->chartOpponent == "senpai");
            Chart loaded;
            SCHECK(loadChartFile(ms->chartPaths[0], ms->diffs[0], loaded, err));
            SCHECK(loaded.title == "Meta Song" && loaded.opponent == "senpai" && std::fabs(loaded.speed - 4.2) < 1e-6);
            SCHECK(loaded.events.size() == 1 && loaded.events[0].name == "Hey!");
        }
        if (bs) {
            SCHECK(bs->diffs.size() == 1 && bs->diffs[0] == "Normal");
            SCHECK(endsWithI(bs->chartPaths[0], "Chart.bin"));
            SCHECK(bs->chartOpponent == "dad");
            Chart loaded;
            SCHECK(loadChartFile(bs->chartPaths[0], bs->diffs[0], loaded, err));
            SCHECK(std::fabs(loaded.speed - 7.0) < 1e-9 && loaded.opponent == "dad" && loaded.player == "bf" &&
                   loaded.gf == "gf" && loaded.stage == "stage");
        }
    }

    fs::remove_all(base);
}

void runScanTests() {
    testIutkCharts();
    testBinaryChartsInMods();
    testFlStudio();
    testPulseForgeCharts();
    testAudioSeconds();
    testModLayouts();
}
