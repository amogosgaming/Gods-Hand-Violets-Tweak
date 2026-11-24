// Astralis Engine. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// iutk Engine binary chart format (.bin, magic "FNC1"):
//   v2 - Legacy flat notes (int32 ms positions + uint8 lanes 0..7)
//   v3 - Deduplicated columnar cells (int32 positions, uint8 lanes, uint64 counts)
//        compatible with iutk's convert-chart.c
//   v4 - Compressed 3-stream RLE + delta + LEB128 varints + IEEE CRC32
//        compatible with iutk's ChartBinary.hx v4
//   v5 - Astralis extension: v4 streams + embedded JSON metadata (optional deflate)
//
// Standard folder layout:
//   [mod]/data/[song]/Chart.bin
//   [mod]/data/[song]/events.json
//   [mod]/data/[song]/metadata.json   (used when Chart.bin has no embedded metadata)
//
// If no metadata is present (neither embedded nor sibling metadata.json),
// Astralis applies defaults: scroll speed 7, opponent "dad", player "bf",
// girlfriend "gf", stage "stage", 120 BPM, 4 keys.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chart.hpp"
#include "chart_binary.hpp"

namespace gh {

constexpr const char* kIutkChartExt = ".bin";
constexpr uint32_t kIutkMaxCells = (1u << 28) - 1;  // 268,435,455 (matches iutk NoteBuffer.MAX_SAFE_CELLS)

// Defaults applied when a Chart.bin has neither embedded metadata nor a sibling metadata.json.
constexpr double kIutkDefaultSpeed = 7.0;
constexpr double kIutkDefaultBpm = 120.0;
constexpr const char* kIutkDefaultOpponent = "dad";
constexpr const char* kIutkDefaultPlayer = "bf";
constexpr const char* kIutkDefaultGf = "gf";
constexpr const char* kIutkDefaultStage = "stage";

enum class IutkVersion : uint8_t {
    V2 = 2,  // Flat notes (5 B/note)
    V3 = 3,  // Columnar deduplicated cells (13 B/cell, convert-chart.c)
    V4 = 4,  // RLE + delta + varint + CRC32 (ChartBinary.hx v4)
    V5 = 5,  // V4 + embedded metadata JSON (Astralis extension)
};

enum class IutkMetaMode : uint8_t {
    SeparateFile = 0,  // Pure iutk Chart.bin + sibling metadata.json
    Embedded = 1,      // Embed metadata inside Chart.bin (writes V5)
    None = 2,          // Notes only (defaults on load: speed 7, dad, bf, gf, stage)
};

enum class IutkRleMode : uint8_t {
    Optimal = 0,  // Exact cost-optimal RLE framing (smallest valid V4 stream)
    Fast = 1,     // Fast greedy RLE (2+ repeats)
    Off = 2,      // Literal runs only (varints without repeat runs)
};

enum class IutkTimeRound : uint8_t {
    Round = 0,     // Nearest millisecond (avoids floating-point truncation drift)
    Truncate = 1,  // Truncate toward zero (exact match with convert-chart.c)
};

inline const char* iutkVersionName(IutkVersion v) {
    switch (v) {
        case IutkVersion::V2: return "v2 (Legacy Flat)";
        case IutkVersion::V3: return "v3 (Raw Cells)";
        case IutkVersion::V4: return "v4 (Compressed RLE)";
        case IutkVersion::V5: return "v5 (V4 + Embedded Meta)";
    }
    return "v4 (Compressed RLE)";
}

struct IutkWriteOptions {
    IutkVersion version = IutkVersion::V4;
    IutkMetaMode metaMode = IutkMetaMode::SeparateFile;
    IutkRleMode rleMode = IutkRleMode::Optimal;
    IutkTimeRound timeRound = IutkTimeRound::Round;
    bool compressEmbeddedMeta = true;  // Deflate embedded metadata JSON (V5)
    bool includeSections = true;       // Store section BPM/mustHit/gf/alt info in metadata
    bool compactJson = false;          // Minify metadata.json / events.json
    bool saveEventsJson = true;        // Write sibling events.json
    bool standardFilename = true;      // Save as Chart.bin (false -> <song>-<diff>.bin)
};

// Lightweight metadata container for writing/inspecting iutk charts.
struct IutkMetadata {
    std::string title;
    std::string difficulty;
    std::string player = kIutkDefaultPlayer;
    std::string opponent = kIutkDefaultOpponent;
    std::string gf = kIutkDefaultGf;
    std::string stage = kIutkDefaultStage;
    double bpm = kIutkDefaultBpm;
    double speed = kIutkDefaultSpeed;
    bool needsVoices = true;
    int keys = 4;
    std::vector<SectionInfo> sections;
    std::vector<TempoPoint> tempo;
};

struct IutkInfo {
    bool valid = false;
    uint8_t version = 0;
    uint32_t cellCount = 0;
    uint64_t totalNotes = 0;
    bool hasEmbeddedMeta = false;
    bool hasSiblingMeta = false;
    IutkMetadata meta;
};

struct IutkWriteResult {
    uint64_t binBytes = 0;
    uint64_t metaBytes = 0;
    uint64_t eventsBytes = 0;
    uint32_t cells = 0;
    uint64_t totalNotes = 0;
    std::string binPath, metaPath, eventsPath;
};

// Fast header check: starts with "FNC1" and a supported version (2..5).
bool isIutkChart(const std::string& path);

// Inspect header + metadata (embedded or sibling metadata.json) without decoding all notes.
bool inspectIutkChart(const std::string& path, IutkInfo& out);

// Load an iutk .bin chart (v2, v3, v4, v5) along with its metadata (embedded or
// sibling metadata.json, falling back to defaults) and sibling events.json.
bool loadIutkChart(const std::string& path, const std::string& difficulty, Chart& out, std::string& err,
                   bool extraKeys = true);

// Build metadata JSON text from IutkMetadata.
std::string buildIutkMetadataJson(const IutkMetadata& meta, bool includeSections = true, bool compact = false);

// Apply metadata JSON text onto an IutkMetadata struct (keeps existing values as defaults).
bool parseIutkMetadataJson(std::string_view jsonText, IutkMetadata& out, std::string_view difficulty = {});

// Write a Chart directly to an iutk .bin file (+ sibling metadata.json and events.json as configured).
bool writeIutkChart(const std::string& binPath, const Chart& chart, const IutkWriteOptions& opt, std::string& err,
                    IutkWriteResult* result = nullptr);

// Write pre-extracted cells/notes with explicit metadata & events (used by the Chart Editor).
struct IutkInputNote {
    double timeMs = 0.0;
    uint8_t side = 0;   // 0 = opponent, 1 = player
    uint8_t lane = 0;   // 0..keys-1 (folded mod 4 into iutk lanes 0..7)
    uint64_t stack = 1;
};

bool writeIutkChartNotes(const std::string& binPath, const std::vector<IutkInputNote>& notes, const IutkMetadata& meta,
                         const std::vector<psych::ChartEvent>& events, const IutkWriteOptions& opt, std::string& err,
                         IutkWriteResult* result = nullptr);

// Convert any chart Astralis reads (Psych JSON, .ghc, V-Slice, PulseForge, FLP, .bin) to iutk .bin.
bool convertChartToIutk(const std::string& srcPath, const std::string& dstBinPath, const IutkWriteOptions& opt,
                        std::string& err, ConvertStats* stats = nullptr, IutkWriteResult* result = nullptr,
                        const std::string& difficulty = "Normal");

}  // namespace gh
