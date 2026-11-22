// Astralis Engine. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// Astralis binary charts (.ghc): a Psych chart in a fraction of its JSON size,
// read many times faster.
//
// What the JSON holds besides the notes (song data, sections, events...) is
// kept as it is, as JSON; the notes are columns (times, lengths, lanes,
// stacks, types) of small numbers - each time as the change of the gap between
// notes, in nanoseconds - packed in blocks that load on every core at once.
#pragma once

#include <cstdint>
#include <string>

#include "chart_stream.hpp"

namespace gh {

constexpr const char* kBinaryChartExt = ".ghc";
constexpr uint32_t kDefaultGhcBlockNotes = 1u << 18;  // 262144 notes per parallel block

enum class GhcCompression : uint8_t {
    None = 0,        // Uncompressed raw columns (fastest load/save)
    Deflate = 1,     // Standard miniz deflate (balanced speed & size)
    RleDeflate = 2,  // Byte RLE pre-pass + miniz deflate (smaller on repetitive/spam charts)
};

inline const char* ghcCompressionName(GhcCompression a) {
    switch (a) {
        case GhcCompression::None: return "None";
        case GhcCompression::Deflate: return "Deflate";
        case GhcCompression::RleDeflate: return "RLE + Deflate";
    }
    return "Deflate";
}

struct GhcWriteOptions {
    GhcCompression algo = GhcCompression::Deflate;
    int level = 3;  // deflate compression strength, 1 (fastest) to 9 (smallest)
    uint32_t blockNotes = kDefaultGhcBlockNotes;
};

// The file starts like a binary chart.
bool isBinaryChart(const std::string& path);

bool writeBinaryChart(const std::string& path, const std::string& skeleton, const StreamedNotes& sn,
                      const GhcWriteOptions& opt, std::string& err);
// Convenience overload: Deflate at `level` (1..9).
bool writeBinaryChart(const std::string& path, const std::string& skeleton, const StreamedNotes& sn, int level,
                      std::string& err);
// What streamChartSkeleton gives for the chart it was made from.
bool readBinaryChart(const std::string& path, std::string& skeleton, StreamedNotes& sn, std::string& err, int threads = -1);
// The JSON part only (song name, characters... for the song list).
bool readBinarySkeleton(const std::string& path, std::string& skeleton);

// Any chart the game reads (Psych JSON, PulseForge...) to a .ghc file.
struct ConvertStats {
    uint64_t inBytes = 0, outBytes = 0, notes = 0;
    double seconds = 0;
};
bool convertChartToBinary(const std::string& src, const std::string& dst, std::string& err, ConvertStats* stats,
                          const GhcWriteOptions& opt);
bool convertChartToBinary(const std::string& src, const std::string& dst, std::string& err, ConvertStats* stats = nullptr,
                          int level = 3);

}  // namespace gh
