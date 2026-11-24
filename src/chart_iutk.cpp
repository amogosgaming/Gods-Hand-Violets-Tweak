// Astralis Engine. Copyright (c) 2026 deatuamae67-crypto.
// All rights reserved; see LICENSE. Forking is not allowed: contribute through issues.

// iutk Engine binary chart format (.bin, magic "FNC1"); see chart_iutk.hpp.
#include "chart_iutk.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chart_stream.hpp"
#include "common.hpp"
#include "json.hpp"
#include "miniz.h"

namespace gh {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'F', 'N', 'C', '1'};
constexpr uint8_t kLanes = 8;
constexpr uint32_t kTimeSignFlip = 0x80000000u;
constexpr uint64_t kMaxV2FlatNotes = 50000000ull;  // 250 MB safety cap for uncompressed flat v2 writes
constexpr uint32_t kMaxV4RunLen = 0x3FFFFFFEu;     // fits safely in signed 31-bit run header

fs::path toPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string pathStr(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Find a file in `dir` matching `targetName` case-insensitively.
std::string findSiblingI(const fs::path& dir, std::string_view targetName) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return {};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) continue;
        if (equalsI(pathStr(it->path().filename()), targetName)) return pathStr(it->path());
    }
    return {};
}

// ---- little-endian & varint helpers ----

inline uint32_t loadU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t loadU64(const uint8_t* p) {
    return uint64_t(loadU32(p)) | (uint64_t(loadU32(p + 4)) << 32);
}

inline void storeU32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v & 0xFFu);
    p[1] = uint8_t((v >> 8) & 0xFFu);
    p[2] = uint8_t((v >> 16) & 0xFFu);
    p[3] = uint8_t((v >> 24) & 0xFFu);
}

inline void storeU64(uint8_t* p, uint64_t v) {
    storeU32(p, uint32_t(v & 0xFFFFFFFFu));
    storeU32(p + 4, uint32_t(v >> 32));
}

inline void appendU32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4];
    storeU32(b, v);
    out.insert(out.end(), b, b + 4);
}

inline int varintSize(uint64_t v) {
    int n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        ++n;
    }
    return n;
}

inline void appendVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80u) {
        out.push_back(uint8_t((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

inline uint32_t crc32Bytes(const uint8_t* data, size_t len) {
    if (len == 0) return 0u;
    return uint32_t(mz_crc32(MZ_CRC32_INIT, data, len));
}

bool readAllFile(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END);
    const long long sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    const long long sz = ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    if (sz < 0) {
        std::fclose(f);
        err = "cannot stat " + path;
        return false;
    }
    out.resize(size_t(sz));
    const size_t rd = sz > 0 ? std::fread(out.data(), 1, size_t(sz), f) : 0;
    std::fclose(f);
    if (rd != size_t(sz)) {
        err = "short read on " + path;
        return false;
    }
    return true;
}

bool writeFileAtomic(const std::string& path, const void* data, size_t size, std::string& err) {
    const std::string tmp = path + ".part";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "cannot write " + path;
        return false;
    }
    bool ok = (size == 0) || (std::fwrite(data, 1, size, f) == size);
    ok = (std::fclose(f) == 0) && ok;
    if (ok) {
        std::remove(path.c_str());
        ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok) {
        std::remove(tmp.c_str());
        err = "cannot write " + path;
    }
    return ok;
}

bool writeTextAtomic(const std::string& path, const std::string& text, std::string& err) {
    return writeFileAtomic(path, text.data(), text.size(), err);
}

// ---- fast pointer-based V4Stream reader (matches ChartBinary.hx V4Stream) ----

class V4MemStream {
public:
    V4MemStream(const uint8_t* data, size_t len, bool raw) : ptr_(data), cur_(data), end_(data + len), raw_(raw) {}

    inline bool readByte(uint8_t& out) {
        if (cur_ >= end_) return false;
        out = *cur_++;
        return true;
    }

    inline bool readFixed32(uint32_t& out) {
        if (size_t(end_ - cur_) < 4) return false;
        out = loadU32(cur_);
        cur_ += 4;
        return true;
    }

    inline bool readVarint(uint64_t& out) {
        if (cur_ < end_ && (*cur_ & 0x80u) == 0) {
            out = *cur_++;
            return true;
        }
        uint64_t v = 0;
        int sh = 0;
        while (cur_ < end_) {
            const uint8_t b = *cur_++;
            if (sh == 63 && b > 1) return false;  // 64-bit overflow check (exact match with ChartBinary.hx)
            v |= uint64_t(b & 0x7Fu) << sh;
            if ((b & 0x80u) == 0) {
                out = v;
                return true;
            }
            sh += 7;
            if (sh > 63) return false;
        }
        return false;
    }

    inline bool readU31(uint32_t& out) {
        uint64_t v = 0;
        if (!readVarint(v) || v > 0x7FFFFFFFull) return false;
        out = uint32_t(v);
        return true;
    }

    inline bool nextItem(uint64_t& out) {
        if (remaining_ == 0) {
            uint64_t hdr = 0;
            if (!readVarint(hdr) || hdr > 0x7FFFFFFFull) return false;
            if ((hdr & 1u) != 0) {
                remaining_ = uint32_t((hdr >> 1) + 2u);
                repeating_ = true;
                if (!readVal(repVal_)) return false;
            } else {
                remaining_ = uint32_t((hdr >> 1) + 1u);
                repeating_ = false;
            }
        }
        if (repeating_) {
            out = repVal_;
        } else {
            if (!readVal(out)) return false;
        }
        --remaining_;
        return true;
    }

    size_t offset() const { return size_t(cur_ - ptr_); }

    bool finish(uint32_t expectedCrc, bool checkCrc) const {
        if (cur_ != end_ || remaining_ != 0) return false;
        if (checkCrc && crc32Bytes(ptr_, size_t(end_ - ptr_)) != expectedCrc) return false;
        return true;
    }

private:
    inline bool readVal(uint64_t& out) {
        if (raw_) {
            uint8_t b = 0;
            if (!readByte(b)) return false;
            out = b;
            return true;
        }
        return readVarint(out);
    }

    const uint8_t* ptr_ = nullptr;
    const uint8_t* cur_ = nullptr;
    const uint8_t* end_ = nullptr;
    bool raw_ = false;
    uint32_t remaining_ = 0;
    bool repeating_ = false;
    uint64_t repVal_ = 0;
};

// ---- V4 stream encoder (Off / Fast / Cost-Optimal DP) ----

struct Run {
    uint64_t val = 0;
    uint32_t len = 0;
};

void emitLiteralRun(std::vector<uint8_t>& out, const std::vector<Run>& runs, size_t from, size_t to, bool raw) {
    // Split if total items exceed kMaxV4RunLen.
    uint64_t total = 0;
    for (size_t i = from; i < to; ++i) total += runs[i].len;
    if (total == 0) return;

    size_t r = from;
    uint32_t remInRun = runs[r].len;
    while (total > 0) {
        const uint32_t chunk = uint32_t(std::min<uint64_t>(total, kMaxV4RunLen));
        appendVarint(out, uint64_t(chunk - 1u) << 1);
        uint32_t left = chunk;
        while (left > 0) {
            const uint32_t take = std::min(left, remInRun);
            const uint64_t val = runs[r].val;
            if (raw) {
                out.insert(out.end(), take, uint8_t(val));
            } else {
                for (uint32_t k = 0; k < take; ++k) appendVarint(out, val);
            }
            left -= take;
            remInRun -= take;
            if (remInRun == 0 && ++r < to) remInRun = runs[r].len;
        }
        total -= chunk;
    }
}

void emitRepeatRun(std::vector<uint8_t>& out, uint64_t val, uint32_t len, bool raw) {
    while (len > 0) {
        if (len == 1) {
            appendVarint(out, 0u);  // literal of 1
            if (raw) out.push_back(uint8_t(val));
            else appendVarint(out, val);
            break;
        }
        const uint32_t chunk = std::min(len, kMaxV4RunLen);
        appendVarint(out, (uint64_t(chunk - 2u) << 1) | 1u);
        if (raw) out.push_back(uint8_t(val));
        else appendVarint(out, val);
        len -= chunk;
    }
}

// Encodes a sequence of values into a valid V4Stream byte buffer.
template <typename T>
std::vector<uint8_t> encodeV4Stream(const std::vector<T>& items, bool raw, IutkRleMode mode) {
    std::vector<uint8_t> out;
    const size_t n = items.size();
    if (n == 0) return out;

    if (mode == IutkRleMode::Off) {
        out.reserve(n + 8);
        size_t pos = 0;
        while (pos < n) {
            const uint32_t chunk = uint32_t(std::min<size_t>(n - pos, kMaxV4RunLen));
            appendVarint(out, uint64_t(chunk - 1u) << 1);
            if (raw) {
                for (uint32_t i = 0; i < chunk; ++i) out.push_back(uint8_t(items[pos + i]));
            } else {
                for (uint32_t i = 0; i < chunk; ++i) appendVarint(out, uint64_t(items[pos + i]));
            }
            pos += chunk;
        }
        return out;
    }

    // Group into maximal equal runs.
    std::vector<Run> runs;
    runs.reserve(std::min<size_t>(n, 65536));
    {
        uint64_t cur = uint64_t(items[0]);
        uint32_t len = 1;
        for (size_t i = 1; i < n; ++i) {
            const uint64_t v = uint64_t(items[i]);
            if (v == cur && len < kMaxV4RunLen) {
                ++len;
            } else {
                runs.push_back({cur, len});
                cur = v;
                len = 1;
            }
        }
        runs.push_back({cur, len});
    }

    const size_t R = runs.size();
    std::vector<uint8_t> asRepeat(R, 0);

    if (mode == IutkRleMode::Fast) {
        for (size_t r = 0; r < R; ++r) {
            if (runs[r].len >= (raw ? 3u : 2u)) asRepeat[r] = 1;
        }
    } else {
        // Exact 2-state DP over maximal equal runs:
        //   state 0 = emit run r inside a literal run
        //   state 1 = emit run r as a repeat run (only valid when len >= 2)
        // Transitioning into state 0 from start or state 1 pays a literal header (~1 byte).
        // Continuing state 0 -> state 0 pays 0 extra header bytes.
        constexpr uint64_t kInf = std::numeric_limits<uint64_t>::max() / 4;
        std::vector<uint8_t> prevLit(R, 0), prevRep(R, 0);
        uint64_t costLit = kInf, costRep = kInf;

        for (size_t r = 0; r < R; ++r) {
            const uint64_t v = runs[r].val;
            const uint32_t len = runs[r].len;
            const uint64_t valBytes = raw ? 1ull : uint64_t(varintSize(v));
            const uint64_t litPayload = uint64_t(len) * valBytes;
            const uint64_t repCost = (len >= 2) ? (uint64_t(varintSize((uint64_t(len - 2u) << 1) | 1u)) + valBytes) : kInf;

            uint64_t nextLit = kInf, nextRep = kInf;
            if (r == 0) {
                nextLit = uint64_t(varintSize(uint64_t(len - 1u) << 1)) + litPayload;
                nextRep = repCost;
            } else {
                // Into literal:
                const uint64_t fromLit = costLit + litPayload;
                const uint64_t fromRep = (costRep < kInf) ? (costRep + uint64_t(varintSize(uint64_t(len - 1u) << 1)) + litPayload) : kInf;
                if (fromLit <= fromRep) {
                    nextLit = fromLit;
                    prevLit[r] = 0;
                } else {
                    nextLit = fromRep;
                    prevLit[r] = 1;
                }
                // Into repeat:
                if (len >= 2) {
                    if (costLit <= costRep) {
                        nextRep = costLit + repCost;
                        prevRep[r] = 0;
                    } else {
                        nextRep = costRep + repCost;
                        prevRep[r] = 1;
                    }
                }
            }
            costLit = nextLit;
            costRep = nextRep;
        }

        uint8_t state = (costRep < costLit) ? 1 : 0;
        for (size_t r = R; r-- > 0;) {
            asRepeat[r] = state;
            state = state ? prevRep[r] : prevLit[r];
        }
    }

    out.reserve(n / 2 + 16);
    size_t i = 0;
    while (i < R) {
        if (asRepeat[i]) {
            emitRepeatRun(out, runs[i].val, runs[i].len, raw);
            ++i;
        } else {
            const size_t start = i;
            while (i < R && !asRepeat[i]) ++i;
            emitLiteralRun(out, runs, start, i, raw);
        }
    }
    return out;
}

// ---- JSON serialization helpers for metadata.json and events.json ----

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", int(c));
            out += buf;
        } else {
            out += char(c);
        }
    }
    out += '"';
    return out;
}

std::string jsonNum(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[48];
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof buf, "%.0f", std::round(v));
    } else {
        std::snprintf(buf, sizeof buf, "%.6g", v);
    }
    return buf;
}

std::string buildEventsJson(const std::vector<psych::ChartEvent>& events, bool compact) {
    // Group events with identical timestamp into Psych format:
    // [[timeMs, [[name, v1, v2], ...]], ...]
    std::vector<psych::ChartEvent> sorted = events;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const psych::ChartEvent& a, const psych::ChartEvent& b) { return a.timeMs < b.timeMs; });
    std::string evs = "[";
    const char* sp = compact ? "" : " ";
    size_t i = 0;
    bool firstGroup = true;
    while (i < sorted.size()) {
        size_t j = i + 1;
        while (j < sorted.size() && std::fabs(sorted[j].timeMs - sorted[i].timeMs) < 1e-6) ++j;
        if (!firstGroup) evs += compact ? "," : ",\n    ";
        else if (!compact) evs += "\n    ";
        firstGroup = false;
        evs += "[" + jsonNum(sorted[i].timeMs) + "," + sp + "[";
        for (size_t k = i; k < j; ++k) {
            if (k > i) evs += "," + std::string(sp);
            evs += "[" + jsonEscape(sorted[k].name) + "," + sp + jsonEscape(sorted[k].v1) + "," + sp +
                   jsonEscape(sorted[k].v2) + "]";
        }
        evs += "]]";
        i = j;
    }
    if (!compact && !firstGroup) evs += "\n  ";
    evs += "]";
    if (compact) return "{\"song\":{\"events\":" + evs + ",\"notes\":[]}}";
    return "{\n  \"song\": {\n    \"events\": " + evs + ",\n    \"notes\": []\n  }\n}\n";
}

// ---- deflate helpers for V5 embedded metadata ----

bool deflateMeta(const std::string& raw, std::vector<uint8_t>& packed) {
    mz_ulong cap = mz_compressBound(mz_ulong(raw.size()));
    packed.resize(size_t(cap));
    if (mz_compress2(packed.data(), &cap, reinterpret_cast<const unsigned char*>(raw.data()), mz_ulong(raw.size()), 6) != MZ_OK)
        return false;
    packed.resize(size_t(cap));
    return true;
}

bool inflateMeta(const uint8_t* src, size_t srcLen, size_t rawLen, std::string& out) {
    out.resize(rawLen);
    if (rawLen == 0) return true;
    mz_ulong m = mz_ulong(rawLen);
    return mz_uncompress(reinterpret_cast<unsigned char*>(out.data()), &m, src, mz_ulong(srcLen)) == MZ_OK && size_t(m) == rawLen;
}

// ---- cell representation & aggregation ----

struct Cell {
    int32_t time = 0;
    uint8_t lane = 0;
    uint64_t count = 0;
};

inline uint64_t makeCellKey(int32_t t, uint8_t lane) {
    return (uint64_t(uint32_t(t) ^ kTimeSignFlip) << 3) | uint64_t(lane & 7u);
}

inline int32_t keyTime(uint64_t key) { return int32_t(uint32_t(key >> 3) ^ kTimeSignFlip); }
inline uint8_t keyLane(uint64_t key) { return uint8_t(key & 7u); }

inline bool addCheckedU64(uint64_t& acc, uint64_t add) {
    if (add > std::numeric_limits<uint64_t>::max() - acc) return false;
    acc += add;
    return true;
}

// Aggregate input notes into sorted, deduplicated (time, lane) -> 64-bit count cells.
bool buildCells(const std::vector<IutkInputNote>& notes, IutkTimeRound rounding, std::vector<Cell>& cells, uint64_t& totalNotes,
                std::string& err) {
    cells.clear();
    totalNotes = 0;
    if (notes.empty()) return true;

    std::unordered_map<uint64_t, uint64_t> table;
    table.reserve(notes.size());

    for (const IutkInputNote& n : notes) {
        if (!std::isfinite(n.timeMs)) continue;
        const double clamped = std::clamp(n.timeMs, double(std::numeric_limits<int32_t>::min()),
                                          double(std::numeric_limits<int32_t>::max()));
        const int32_t t = (rounding == IutkTimeRound::Truncate) ? int32_t(int64_t(clamped)) : int32_t(std::llround(clamped));
        const uint8_t lane = uint8_t((n.side ? 4u : 0u) + (n.lane % 4u));
        const uint64_t stack = std::max<uint64_t>(1, n.stack);
        if (!addCheckedU64(totalNotes, stack)) {
            err = "total note count overflows 64 bits";
            return false;
        }
        const uint64_t key = makeCellKey(t, lane);
        auto it = table.find(key);
        if (it == table.end()) {
            table.emplace(key, stack);
        } else {
            if (!addCheckedU64(it->second, stack)) {
                err = "cell note count overflows 64 bits";
                return false;
            }
        }
    }

    if (table.size() > kIutkMaxCells) {
        err = "chart exceeds iutk cell limit (" + std::to_string(kIutkMaxCells) + " cells)";
        return false;
    }

    std::vector<std::pair<uint64_t, uint64_t>> pairs;
    pairs.reserve(table.size());
    for (const auto& kv : table) pairs.push_back(kv);
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    cells.reserve(pairs.size());
    for (const auto& kv : pairs) {
        cells.push_back({keyTime(kv.first), keyLane(kv.first), kv.second});
    }
    return true;
}

// ---- V4/V5 header parsing helper ----

struct V4Header {
    uint8_t version = 4;
    uint32_t cellCount = 0;
    uint32_t groupCount = 0;
    uint64_t totalNotes = 0;
    uint32_t firstTime = 0;
    uint32_t lenD = 0, lenM = 0, lenC = 0;
    uint32_t crcD = 0, crcM = 0, crcC = 0;
    // V5 embedded metadata fields:
    uint32_t metaFlags = 0;
    uint32_t metaRawLen = 0;
    uint32_t metaPackedLen = 0;
    uint32_t metaCrc = 0;
    size_t bodyStart = 0;
};

bool parseV4OrV5Header(const uint8_t* data, size_t size, uint8_t version, V4Header& h, std::string& err) {
    if (size < 5) {
        err = "truncated iutk binary chart";
        return false;
    }
    h = V4Header{};
    h.version = version;
    V4MemStream s(data + 5, size - 5, false);
    uint64_t firstT = 0;
    if (!s.readU31(h.cellCount) || !s.readU31(h.groupCount) || !s.readVarint(h.totalNotes) || !s.readVarint(firstT) ||
        firstT > 0xFFFFFFFFull || !s.readU31(h.lenD) || !s.readU31(h.lenM) || !s.readU31(h.lenC) ||
        !s.readFixed32(h.crcD) || !s.readFixed32(h.crcM) || !s.readFixed32(h.crcC)) {
        err = "corrupt iutk binary chart header";
        return false;
    }
    h.firstTime = uint32_t(firstT);
    if (version == 5) {
        uint64_t mf = 0;
        if (!s.readVarint(mf) || mf > 0xFFu || !s.readU31(h.metaRawLen) || !s.readU31(h.metaPackedLen) ||
            !s.readFixed32(h.metaCrc)) {
            err = "corrupt iutk v5 metadata header";
            return false;
        }
        h.metaFlags = uint32_t(mf);
    }
    h.bodyStart = 5 + s.offset();
    const uint64_t expected = uint64_t(h.bodyStart) + h.lenD + h.lenM + h.lenC + h.metaPackedLen;
    if (expected != uint64_t(size)) {
        err = "corrupt or truncated iutk binary chart (size mismatch)";
        return false;
    }
    if (h.cellCount > kIutkMaxCells) {
        err = "iutk chart exceeds cell limit";
        return false;
    }
    if (h.groupCount > h.cellCount || (h.groupCount == 0) != (h.cellCount == 0) ||
        uint64_t(h.cellCount) > uint64_t(h.groupCount) * kLanes) {
        err = "inconsistent cell and group counts in iutk binary chart";
        return false;
    }
    return true;
}

bool extractV5Meta(const uint8_t* data, const V4Header& h, std::string& metaJson, std::string& err) {
    metaJson.clear();
    if (h.version != 5 || h.metaRawLen == 0) return true;
    const uint8_t* ptr = data + h.bodyStart + h.lenD + h.lenM + h.lenC;
    if (crc32Bytes(ptr, h.metaPackedLen) != h.metaCrc) {
        err = "corrupt iutk v5 metadata checksum";
        return false;
    }
    if ((h.metaFlags & 1u) != 0) {
        if (!inflateMeta(ptr, h.metaPackedLen, h.metaRawLen, metaJson)) {
            err = "failed to decompress iutk v5 metadata";
            return false;
        }
    } else {
        if (h.metaPackedLen != h.metaRawLen) {
            err = "corrupt iutk v5 raw metadata length";
            return false;
        }
        metaJson.assign(reinterpret_cast<const char*>(ptr), h.metaPackedLen);
    }
    return true;
}

void applyMetadataToChart(const IutkMetadata& m, const std::string& fallbackTitle, const std::string& fallbackDiff,
                          Chart& out, double& baseBpm) {
    out.title = !m.title.empty() ? m.title : fallbackTitle;
    out.difficulty = !m.difficulty.empty() ? m.difficulty : fallbackDiff;
    out.player = !m.player.empty() ? m.player : kIutkDefaultPlayer;
    out.opponent = !m.opponent.empty() ? m.opponent : kIutkDefaultOpponent;
    out.gf = !m.gf.empty() ? m.gf : kIutkDefaultGf;
    out.stage = !m.stage.empty() ? m.stage : kIutkDefaultStage;
    out.speed = (m.speed > 0.0 && std::isfinite(m.speed)) ? m.speed : kIutkDefaultSpeed;
    out.keys = std::clamp(m.keys > 0 ? m.keys : 4, 1, 18);
    baseBpm = (m.bpm > 0.0 && std::isfinite(m.bpm)) ? m.bpm : kIutkDefaultBpm;

    out.tempo.clear();
    out.focus.clear();
    out.sections.clear();

    if (!m.tempo.empty()) {
        out.tempo = m.tempo;
    }
    if (!m.sections.empty()) {
        out.sections = m.sections;
        double curBpm = baseBpm;
        double secStart = 0.0;
        if (out.tempo.empty()) out.tempo.push_back({0.0, curBpm, 0.0});
        for (SectionInfo& si : out.sections) {
            si.timeMs = secStart;
            if (si.bpm > 0.0 && std::isfinite(si.bpm) && std::fabs(si.bpm - curBpm) > 1e-9) {
                curBpm = si.bpm;
                if (m.tempo.empty()) out.tempo.push_back({secStart, curBpm, 0.0});
            } else {
                si.bpm = curBpm;
            }
            const double beats = (si.beats > 0.0 && std::isfinite(si.beats)) ? si.beats : 4.0;
            si.beats = beats;
            const uint8_t fside = si.mustHit ? 1 : 0;
            if (out.focus.empty() || out.focus.back().side != fside) out.focus.push_back({secStart, fside});
            secStart += beats * 60000.0 / curBpm;
        }
    }
}

IutkMetadata metadataFromChart(const Chart& c) {
    IutkMetadata m;
    m.title = c.title;
    m.difficulty = c.difficulty;
    m.player = c.player.empty() ? kIutkDefaultPlayer : c.player;
    m.opponent = c.opponent.empty() ? kIutkDefaultOpponent : c.opponent;
    m.gf = c.gf.empty() ? kIutkDefaultGf : c.gf;
    m.stage = c.stage.empty() ? kIutkDefaultStage : c.stage;
    m.bpm = c.tempo.empty() ? kIutkDefaultBpm : c.bpmAt(0);
    m.speed = (c.speed > 0.0 && std::isfinite(c.speed)) ? c.speed : kIutkDefaultSpeed;
    m.keys = c.keys > 0 ? c.keys : 4;
    m.sections = c.sections;
    m.tempo = c.tempo;
    return m;
}

}  // namespace

// --------------------------------------------------------------- detection --

bool isIutkChart(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t head[5]{};
    const bool ok = (std::fread(head, 1, 5, f) == 5) && std::memcmp(head, kMagic, 4) == 0 &&
                    (head[4] >= 2 && head[4] <= 5);
    std::fclose(f);
    return ok;
}

// ------------------------------------------------------------ metadata JSON --

std::string buildIutkMetadataJson(const IutkMetadata& meta, bool includeSections, bool compact) {
    const char* nl = compact ? "" : "\n";
    const char* in1 = compact ? "" : "  ";
    const char* in2 = compact ? "" : "    ";
    const char* sp = compact ? "" : " ";
    std::string o;
    o.reserve(512 + (includeSections ? meta.sections.size() * 80 : 0));
    o += "{";
    o += nl;
    o += std::string(in1) + "\"format\":" + sp + "\"iutk_astralis\"," + nl;
    o += std::string(in1) + "\"song\":" + sp + jsonEscape(meta.title) + "," + nl;
    if (!meta.difficulty.empty())
        o += std::string(in1) + "\"difficulty\":" + sp + jsonEscape(meta.difficulty) + "," + nl;
    o += std::string(in1) + "\"bpm\":" + sp + jsonNum(meta.bpm > 0 ? meta.bpm : kIutkDefaultBpm) + "," + nl;
    o += std::string(in1) + "\"speed\":" + sp + jsonNum(meta.speed > 0 ? meta.speed : kIutkDefaultSpeed) + "," + nl;
    o += std::string(in1) + "\"player1\":" + sp + jsonEscape(meta.player.empty() ? kIutkDefaultPlayer : meta.player) + "," + nl;
    o += std::string(in1) + "\"player2\":" + sp + jsonEscape(meta.opponent.empty() ? kIutkDefaultOpponent : meta.opponent) + "," + nl;
    o += std::string(in1) + "\"gfVersion\":" + sp + jsonEscape(meta.gf.empty() ? kIutkDefaultGf : meta.gf) + "," + nl;
    o += std::string(in1) + "\"stage\":" + sp + jsonEscape(meta.stage.empty() ? kIutkDefaultStage : meta.stage) + "," + nl;
    o += std::string(in1) + "\"needsVoices\":" + sp + (meta.needsVoices ? "true" : "false") + "," + nl;
    o += std::string(in1) + "\"keys\":" + sp + std::to_string(meta.keys > 0 ? meta.keys : 4);

    if (includeSections && !meta.sections.empty()) {
        o += "," + std::string(nl) + in1 + "\"sections\":" + sp + "[" + nl;
        double prevBpm = meta.bpm > 0 ? meta.bpm : kIutkDefaultBpm;
        for (size_t i = 0; i < meta.sections.size(); ++i) {
            const SectionInfo& s = meta.sections[i];
            const double bpm = s.bpm > 0 ? s.bpm : prevBpm;
            const bool chBpm = (i == 0 && std::fabs(bpm - prevBpm) > 1e-6) || (i > 0 && std::fabs(bpm - prevBpm) > 1e-6);
            prevBpm = bpm;
            const double beats = s.beats > 0 ? s.beats : 4.0;
            o += in2;
            o += "{\"mustHitSection\":" + std::string(sp) + (s.mustHit ? "true" : "false");
            o += ",\"sectionBeats\":" + std::string(sp) + jsonNum(beats);
            if (chBpm) {
                o += ",\"bpm\":" + std::string(sp) + jsonNum(bpm);
                o += ",\"changeBPM\":" + std::string(sp) + "true";
            }
            if (s.gf) o += ",\"gfSection\":" + std::string(sp) + "true";
            if (s.alt) o += ",\"altAnim\":" + std::string(sp) + "true";
            o += "}";
            if (i + 1 < meta.sections.size()) o += ",";
            o += nl;
        }
        o += std::string(in1) + "]";
    } else if (!meta.tempo.empty() && meta.tempo.size() > 1) {
        o += "," + std::string(nl) + in1 + "\"tempo\":" + sp + "[";
        for (size_t i = 0; i < meta.tempo.size(); ++i) {
            if (i > 0) o += ",";
            o += "{\"timeMs\":" + jsonNum(meta.tempo[i].timeMs) + ",\"bpm\":" + jsonNum(meta.tempo[i].bpm) + "}";
        }
        o += "]";
    }

    o += nl;
    o += "}";
    if (!compact) o += "\n";
    return o;
}

bool parseIutkMetadataJson(std::string_view jsonText, IutkMetadata& out, std::string_view difficulty) {
    json::Document doc;
    if (!doc.parse(jsonText)) return false;
    const json::Value& root = doc.root();
    if (root.type != json::Type::Object) return false;

    // Support both flat root objects and Psych-style {"song": {...}} wrapper.
    const json::Value* songObj = doc.get(root, "song");
    const json::Value& s = (songObj && songObj->type == json::Type::Object) ? *songObj : root;

    auto pickStr = [&](std::initializer_list<const char*> keys, std::string& dst) {
        for (const char* k : keys) {
            if (const json::Value* v = doc.get(s, k); v && v->type == json::Type::String) {
                std::string val = trim(doc.str(*v));
                if (!val.empty()) {
                    dst = std::move(val);
                    return;
                }
            }
        }
    };

    pickStr({"song", "songName", "title"}, out.title);
    pickStr({"difficulty"}, out.difficulty);
    pickStr({"player1", "player", "bf"}, out.player);
    pickStr({"player2", "opponent", "dad"}, out.opponent);
    pickStr({"gfVersion", "gf", "player3", "girlfriend"}, out.gf);
    pickStr({"stage"}, out.stage);

    // V-Slice playData.characters / playData.stage support
    if (const json::Value* pd = doc.get(root, "playData"); pd && pd->type == json::Type::Object) {
        if (const json::Value* ch = doc.get(*pd, "characters"); ch && ch->type == json::Type::Object) {
            if (const json::Value* v = doc.get(*ch, "player"); v && v->type == json::Type::String && !doc.str(*v).empty())
                out.player = std::string(doc.str(*v));
            if (const json::Value* v = doc.get(*ch, "opponent"); v && v->type == json::Type::String && !doc.str(*v).empty())
                out.opponent = std::string(doc.str(*v));
            if (const json::Value* v = doc.get(*ch, "girlfriend"); v && v->type == json::Type::String && !doc.str(*v).empty())
                out.gf = std::string(doc.str(*v));
        }
        if (const json::Value* st = doc.get(*pd, "stage"); st && st->type == json::Type::String && !doc.str(*st).empty())
            out.stage = std::string(doc.str(*st));
    }

    if (const json::Value* v = doc.get(s, "bpm")) {
        const double b = json::Document::num(v, 0.0);
        if (b > 0.0 && std::isfinite(b)) out.bpm = b;
    }
    if (const json::Value* v = doc.get(s, "speed")) {
        const double sp = json::Document::num(v, 0.0);
        if (sp > 0.0 && std::isfinite(sp)) out.speed = sp;
    } else if (const json::Value* v = doc.get(s, "scrollSpeed")) {
        if (v->type == json::Type::Number) {
            const double sp = json::Document::num(v, 0.0);
            if (sp > 0.0 && std::isfinite(sp)) out.speed = sp;
        } else if (v->type == json::Type::Object) {
            const std::string dl = toLower(difficulty);
            const json::Value* dv = !dl.empty() ? doc.get(*v, dl) : nullptr;
            if (!dv) dv = doc.get(*v, "default");
            if (!dv && doc.size(*v) > 0) dv = &doc.valAt(*v, 0);
            const double sp = json::Document::num(dv, 0.0);
            if (sp > 0.0 && std::isfinite(sp)) out.speed = sp;
        }
    }
    if (const json::Value* v = doc.get(s, "needsVoices")) out.needsVoices = json::Document::boolean(v, true);
    if (const json::Value* v = doc.get(s, "keys")) {
        const int k = int(json::Document::num(v, 0));
        if (k >= 1 && k <= 18) out.keys = k;
    } else if (const json::Value* v = doc.get(s, "keyCount")) {
        const int k = int(json::Document::num(v, 0));
        if (k >= 1 && k <= 18) out.keys = k;
    }

    // Tempo points: "tempo" or V-Slice "timeChanges"
    out.tempo.clear();
    if (const json::Value* tc = doc.get(s, "tempo"); tc && tc->type == json::Type::Array) {
        for (size_t i = 0; i < doc.size(*tc); ++i) {
            const json::Value& item = doc.at(*tc, i);
            if (item.type != json::Type::Object) continue;
            const double tm = json::Document::num(doc.get(item, "timeMs"), json::Document::num(doc.get(item, "t"), 0.0));
            const double bm = json::Document::num(doc.get(item, "bpm"), 0.0);
            if (bm > 0.0 && std::isfinite(bm) && std::isfinite(tm)) out.tempo.push_back({tm, bm, 0.0});
        }
    } else if (const json::Value* tc = doc.get(root, "timeChanges"); tc && tc->type == json::Type::Array) {
        for (size_t i = 0; i < doc.size(*tc); ++i) {
            const json::Value& item = doc.at(*tc, i);
            if (item.type != json::Type::Object) continue;
            const double tm = json::Document::num(doc.get(item, "t"), 0.0);
            const double bm = json::Document::num(doc.get(item, "bpm"), 0.0);
            if (bm > 0.0 && std::isfinite(bm) && std::isfinite(tm)) out.tempo.push_back({tm, bm, 0.0});
        }
    }
    if (!out.tempo.empty() && out.tempo[0].bpm > 0.0) out.bpm = out.tempo[0].bpm;

    // Sections (optional)
    out.sections.clear();
    const json::Value* secs = doc.get(s, "sections");
    if (!secs || secs->type != json::Type::Array) secs = doc.get(s, "notes");
    if (secs && secs->type == json::Type::Array) {
        double curBpm = out.bpm > 0 ? out.bpm : kIutkDefaultBpm;
        double secStart = 0.0;
        for (size_t i = 0; i < doc.size(*secs); ++i) {
            const json::Value& sec = doc.at(*secs, i);
            if (sec.type != json::Type::Object) continue;
            SectionInfo si;
            si.timeMs = secStart;
            si.mustHit = json::Document::boolean(doc.get(sec, "mustHitSection"), true);
            si.gf = json::Document::boolean(doc.get(sec, "gfSection"), false);
            si.alt = json::Document::boolean(doc.get(sec, "altAnim"), false);
            if (json::Document::boolean(doc.get(sec, "changeBPM"), false)) {
                const double nb = json::Document::num(doc.get(sec, "bpm"), curBpm);
                if (nb > 0.0 && std::isfinite(nb)) curBpm = nb;
            }
            si.bpm = curBpm;
            double beats = json::Document::num(doc.get(sec, "sectionBeats"), -1.0);
            if (!(beats > 0.0) || !std::isfinite(beats)) {
                beats = json::Document::num(doc.get(sec, "lengthInSteps"), 16.0) / 4.0;
                if (!(beats > 0.0) || !std::isfinite(beats)) beats = 4.0;
            }
            si.beats = beats;
            out.sections.push_back(si);
            secStart += beats * 60000.0 / curBpm;
        }
    }
    return true;
}

// --------------------------------------------------------------- inspect --

bool inspectIutkChart(const std::string& path, IutkInfo& out) {
    out = IutkInfo{};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END);
    const long long fsz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    const long long fsz = ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    if (fsz < 5) {
        std::fclose(f);
        return false;
    }
    uint8_t head[128]{};
    const size_t rd = std::fread(head, 1, std::min<size_t>(sizeof head, size_t(fsz)), f);
    if (rd < 5 || std::memcmp(head, kMagic, 4) != 0) {
        std::fclose(f);
        return false;
    }
    const uint8_t ver = head[4];
    if (ver < 2 || ver > 5) {
        std::fclose(f);
        return false;
    }
    out.version = ver;
    if (ver == 2) {
        if (rd < 9) { std::fclose(f); return false; }
        const int32_t nc = int32_t(loadU32(head + 5));
        if (nc < 0 || uint64_t(fsz) < 9ull + uint64_t(nc) * 5ull) { std::fclose(f); return false; }
        out.cellCount = uint32_t(nc);
        out.totalNotes = uint64_t(nc);
        out.valid = true;
    } else if (ver == 3) {
        if (rd < 17) { std::fclose(f); return false; }
        const uint32_t cc = loadU32(head + 5);
        const uint64_t tot = loadU64(head + 9);
        if (cc > kIutkMaxCells || uint64_t(fsz) < 17ull + uint64_t(cc) * 13ull) { std::fclose(f); return false; }
        out.cellCount = cc;
        out.totalNotes = tot;
        out.valid = true;
    } else {
        // V4 / V5: header fits well within 128 bytes.
        V4MemStream s(head + 5, rd - 5, false);
        uint32_t cc = 0, gc = 0, lenD = 0, lenM = 0, lenC = 0, crcD = 0, crcM = 0, crcC = 0;
        uint64_t tot = 0, firstT = 0;
        if (!s.readU31(cc) || !s.readU31(gc) || !s.readVarint(tot) || !s.readVarint(firstT) || firstT > 0xFFFFFFFFull ||
            !s.readU31(lenD) || !s.readU31(lenM) || !s.readU31(lenC) || !s.readFixed32(crcD) || !s.readFixed32(crcM) ||
            !s.readFixed32(crcC)) {
            std::fclose(f);
            return false;
        }
        uint32_t metaFlags = 0, metaRawLen = 0, metaPackedLen = 0, metaCrc = 0;
        if (ver == 5) {
            uint64_t mf = 0;
            if (!s.readVarint(mf) || mf > 0xFFu || !s.readU31(metaRawLen) || !s.readU31(metaPackedLen) ||
                !s.readFixed32(metaCrc)) {
                std::fclose(f);
                return false;
            }
            metaFlags = uint32_t(mf);
        }
        const uint64_t bodyStart = 5ull + s.offset();
        if (bodyStart + lenD + lenM + lenC + metaPackedLen != uint64_t(fsz)) {
            std::fclose(f);
            return false;
        }
        out.cellCount = cc;
        out.totalNotes = tot;
        out.valid = true;
        if (ver == 5 && metaRawLen > 0 && metaPackedLen > 0) {
            const uint64_t metaOff = bodyStart + lenD + lenM + lenC;
#if defined(_WIN32)
            const bool sought = _fseeki64(f, (long long)metaOff, SEEK_SET) == 0;
#else
            const bool sought = fseeko(f, off_t(metaOff), SEEK_SET) == 0;
#endif
            if (sought) {
                std::vector<uint8_t> mb(metaPackedLen);
                if (std::fread(mb.data(), 1, metaPackedLen, f) == metaPackedLen &&
                    crc32Bytes(mb.data(), metaPackedLen) == metaCrc) {
                    std::string mjson;
                    bool okMeta = false;
                    if ((metaFlags & 1u) != 0) okMeta = inflateMeta(mb.data(), metaPackedLen, metaRawLen, mjson);
                    else if (metaPackedLen == metaRawLen) {
                        mjson.assign(reinterpret_cast<const char*>(mb.data()), metaPackedLen);
                        okMeta = true;
                    }
                    if (okMeta && parseIutkMetadataJson(mjson, out.meta)) out.hasEmbeddedMeta = true;
                }
            }
        }
    }
    std::fclose(f);

    if (!out.hasEmbeddedMeta) {
        const fs::path dir = toPath(path).parent_path();
        const std::string metaPath = findSiblingI(dir, "metadata.json");
        if (!metaPath.empty()) {
            std::string metaText;
            if (readTextFile(metaPath, metaText) && parseIutkMetadataJson(metaText, out.meta)) {
                out.hasSiblingMeta = true;
            }
        }
    }
    return out.valid;
}

// ----------------------------------------------------------------- loader --

namespace {

bool loadV2Body(const uint8_t* data, size_t size, Chart& out, std::string& err) {
    if (size < 9) {
        err = "corrupt or truncated iutk v2 chart";
        return false;
    }
    const int32_t noteCount = int32_t(loadU32(data + 5));
    if (noteCount < 0) {
        err = "negative note count in iutk v2 chart";
        return false;
    }
    const uint64_t required = 9ull + uint64_t(noteCount) * 5ull;
    if (required > uint64_t(size)) {
        err = "truncated iutk v2 chart";
        return false;
    }
    const uint8_t* posPtr = data + 9;
    const uint8_t* lanePtr = data + 9 + size_t(noteCount) * 4;

    // Coalesce consecutive notes at the same timestamp into deduplicated cells,
    // matching ChartBinary.hx loadV2.
    out.notes.clear();
    out.notes.reserve(std::min<size_t>(size_t(noteCount), 1u << 16));
    uint64_t runCounts[kLanes]{};
    bool runValid = false;
    int32_t runPos = 0;

    auto flushRun = [&]() {
        for (uint8_t lane = 0; lane < kLanes; ++lane) {
            if (runCounts[lane] == 0) continue;
            Note n;
            n.time = double(runPos);
            n.length = 0.0;
            n.side = lane >= 4 ? 1 : 0;
            n.lane = uint8_t(lane & 3u);
            n.stack = runCounts[lane];
            out.notes.push_back(n);
            runCounts[lane] = 0;
        }
    };

    for (int32_t i = 0; i < noteCount; ++i) {
        const int32_t p = int32_t(loadU32(posPtr + size_t(i) * 4));
        const uint8_t lane = lanePtr[i];
        if (lane >= kLanes) {
            err = "corrupt iutk v2 chart: lane out of range";
            return false;
        }
        if (!runValid || p != runPos) {
            if (runValid) flushRun();
            runValid = true;
            runPos = p;
        }
        ++runCounts[lane];
    }
    if (runValid) flushRun();
    out.rawNoteCount = uint64_t(noteCount);
    return true;
}

bool loadV3Body(const uint8_t* data, size_t size, Chart& out, std::string& err) {
    if (size < 17) {
        err = "corrupt or truncated iutk v3 chart";
        return false;
    }
    const uint32_t cellCount = loadU32(data + 5);
    const uint64_t declared = loadU64(data + 9);
    if (cellCount > kIutkMaxCells) {
        err = "iutk v3 chart exceeds safe cell limit";
        return false;
    }
    const uint64_t required = 17ull + uint64_t(cellCount) * 13ull;
    if (required > uint64_t(size)) {
        err = "truncated iutk v3 chart";
        return false;
    }
    const uint8_t* posPtr = data + 17;
    const uint8_t* lanePtr = posPtr + size_t(cellCount) * 4;
    const uint8_t* cntPtr = lanePtr + size_t(cellCount);

    out.notes.clear();
    out.notes.resize(cellCount);
    uint64_t sum = 0;
    size_t written = 0;
    for (uint32_t i = 0; i < cellCount; ++i) {
        const int32_t p = int32_t(loadU32(posPtr + size_t(i) * 4));
        const uint8_t lane = lanePtr[i];
        if (lane >= kLanes) {
            err = "corrupt iutk v3 chart: lane out of range";
            return false;
        }
        const uint64_t count = loadU64(cntPtr + size_t(i) * 8);
        if (!addCheckedU64(sum, count)) {
            err = "iutk v3 chart total note count overflows 64 bits";
            return false;
        }
        if (count == 0) continue;
        Note& n = out.notes[written++];
        n.time = double(p);
        n.length = 0.0;
        n.side = lane >= 4 ? 1 : 0;
        n.lane = uint8_t(lane & 3u);
        n.kind = kNormal;
        n.flags = 0;
        n.type = 0;
        n.stack = count;
    }
    if (sum != declared) {
        err = "corrupt iutk v3 chart: declared note total mismatch";
        return false;
    }
    out.notes.resize(written);
    out.rawNoteCount = sum;
    return true;
}

bool loadV4OrV5Body(const uint8_t* data, size_t size, uint8_t version, Chart& out, std::string& embeddedMeta, std::string& err) {
    V4Header h;
    if (!parseV4OrV5Header(data, size, version, h, err)) return false;

    const uint8_t* ptrD = data + h.bodyStart;
    const uint8_t* ptrM = ptrD + h.lenD;
    const uint8_t* ptrC = ptrM + h.lenM;

    // Parallel CRC32 verification for large compressed charts (over 128 KB total streams).
    const size_t totalStreamBytes = size_t(h.lenD) + size_t(h.lenM) + size_t(h.lenC);
    bool crcOk = false;
    if (totalStreamBytes >= (128u << 10)) {
        auto fD = std::async(std::launch::async, [=] { return crc32Bytes(ptrD, h.lenD) == h.crcD; });
        auto fM = std::async(std::launch::async, [=] { return crc32Bytes(ptrM, h.lenM) == h.crcM; });
        const bool okC = (crc32Bytes(ptrC, h.lenC) == h.crcC);
        const bool okM = fM.get();
        const bool okD = fD.get();
        crcOk = okD && okM && okC;
    } else {
        crcOk = (crc32Bytes(ptrD, h.lenD) == h.crcD) && (crc32Bytes(ptrM, h.lenM) == h.crcM) &&
                (crc32Bytes(ptrC, h.lenC) == h.crcC);
    }
    if (!crcOk) {
        err = "corrupt iutk chart: CRC32 checksum mismatch";
        return false;
    }

    if (version == 5 && !extractV5Meta(data, h, embeddedMeta, err)) return false;

    V4MemStream sD(ptrD, h.lenD, false);
    V4MemStream sM(ptrM, h.lenM, true);
    V4MemStream sC(ptrC, h.lenC, false);

    out.notes.clear();
    out.notes.resize(h.cellCount);
    uint64_t sum = 0;
    uint64_t timeU = h.firstTime;
    uint32_t cells = 0;

    for (uint32_t g = 0; g < h.groupCount; ++g) {
        if (g > 0) {
            uint64_t dv = 0;
            if (!sD.nextItem(dv) || dv > 0xFFFFFFFFull) {
                err = "corrupt iutk chart: time delta out of range";
                return false;
            }
            timeU += dv + 1ull;
            if (timeU > 0xFFFFFFFFull) {
                err = "corrupt iutk chart: time overflow";
                return false;
            }
        }
        uint64_t mask = 0;
        if (!sM.nextItem(mask) || mask < 1 || mask > 255) {
            err = "corrupt iutk chart: invalid lane mask";
            return false;
        }
        const int32_t pos = int32_t(uint32_t(timeU) ^ kTimeSignFlip);
        const double posMs = double(pos);

        // Iterate only set bits in mask (ascending 0..7) — preserves exact (side, lane) order.
        uint32_t bits = uint32_t(mask);
        while (bits) {
            const uint8_t lane = uint8_t(std::countr_zero(bits));
            bits &= bits - 1u;
            if (cells >= h.cellCount) {
                err = "corrupt iutk chart: more cells than declared";
                return false;
            }
            uint64_t cv = 0;
            if (!sC.nextItem(cv) || cv == std::numeric_limits<uint64_t>::max()) {
                err = "corrupt iutk chart: cell count overflows 64 bits";
                return false;
            }
            const uint64_t count = cv + 1ull;
            if (!addCheckedU64(sum, count)) {
                err = "corrupt iutk chart: total note count overflows 64 bits";
                return false;
            }
            Note& n = out.notes[cells++];
            n.time = posMs;
            n.length = 0.0;
            n.side = lane >= 4 ? 1 : 0;
            n.lane = uint8_t(lane & 3u);
            n.kind = kNormal;
            n.flags = 0;
            n.type = 0;
            n.stack = count;
        }
    }

    if (cells != h.cellCount) {
        err = "corrupt iutk chart: cell count mismatch";
        return false;
    }
    if (sum != h.totalNotes) {
        err = "corrupt iutk chart: total note count mismatch";
        return false;
    }
    if (!sD.finish(h.crcD, false) || !sM.finish(h.crcM, false) || !sC.finish(h.crcC, false)) {
        err = "corrupt iutk chart: stream has unread or missing data";
        return false;
    }

    out.rawNoteCount = sum;
    return true;
}

}  // namespace

bool loadIutkChart(const std::string& path, const std::string& difficulty, Chart& out, std::string& err, bool /*extraKeys*/) {
    out = Chart{};
    std::vector<uint8_t> bytes;
    if (!readAllFile(path, bytes, err)) return false;
    if (bytes.size() < 5 || std::memcmp(bytes.data(), kMagic, 4) != 0) {
        err = "not an iutk binary chart: " + path;
        return false;
    }
    const uint8_t ver = bytes[4];
    std::string embeddedMeta;
    bool ok = false;
    if (ver == 2) ok = loadV2Body(bytes.data(), bytes.size(), out, err);
    else if (ver == 3) ok = loadV3Body(bytes.data(), bytes.size(), out, err);
    else if (ver == 4 || ver == 5) ok = loadV4OrV5Body(bytes.data(), bytes.size(), ver, out, embeddedMeta, err);
    else {
        err = "unsupported iutk binary chart version: " + std::to_string(int(ver));
        return false;
    }
    if (!ok) {
        out = Chart{};
        return false;
    }

    // Metadata resolution:
    //   1. Defaults (scroll speed 7, dad, bf, gf, stage, 120 BPM, 4 keys)
    //   2. Embedded metadata (V5) if present
    //   3. Otherwise sibling metadata.json in the same directory
    IutkMetadata meta;
    const fs::path p = toPath(path);
    const fs::path dir = p.parent_path();
    const std::string fallbackTitle = prettyName(pathStr(dir.filename()));

    if (!embeddedMeta.empty()) {
        parseIutkMetadataJson(embeddedMeta, meta, difficulty);
    } else {
        const std::string metaPath = findSiblingI(dir, "metadata.json");
        if (!metaPath.empty()) {
            std::string metaText;
            if (readTextFile(metaPath, metaText)) parseIutkMetadataJson(metaText, meta, difficulty);
        }
    }

    double baseBpm = kIutkDefaultBpm;
    applyMetadataToChart(meta, fallbackTitle, difficulty.empty() ? "Normal" : difficulty, out, baseBpm);

    // Load sibling events.json if present.
    const std::string evPath = findSiblingI(dir, "events.json");
    if (!evPath.empty()) {
        std::string evText;
        if (readTextFile(evPath, evText)) {
            psych::parseEvents(evText, out.events);
            std::stable_sort(out.events.begin(), out.events.end(),
                             [](const psych::ChartEvent& a, const psych::ChartEvent& b) { return a.timeMs < b.timeMs; });
        }
    }

    out.finalize(baseBpm);
    return true;
}

// ----------------------------------------------------------------- writer --

namespace {

bool writeV2File(const std::string& path, const std::vector<Cell>& cells, uint64_t totalNotes, std::string& err) {
    if (totalNotes > kMaxV2FlatNotes) {
        err = "too many notes (" + std::to_string(totalNotes) + ") for flat iutk v2; use v3 or v4 instead";
        return false;
    }
    const size_t n = size_t(totalNotes);
    std::vector<uint8_t> buf(9 + n * 5);
    std::memcpy(buf.data(), kMagic, 4);
    buf[4] = 2;
    storeU32(buf.data() + 5, uint32_t(n));
    uint8_t* posOut = buf.data() + 9;
    uint8_t* laneOut = posOut + n * 4;
    size_t idx = 0;
    for (const Cell& c : cells) {
        for (uint64_t k = 0; k < c.count; ++k) {
            storeU32(posOut + idx * 4, uint32_t(c.time));
            laneOut[idx] = c.lane;
            ++idx;
        }
    }
    return writeFileAtomic(path, buf.data(), buf.size(), err);
}

bool writeV3File(const std::string& path, const std::vector<Cell>& cells, uint64_t totalNotes, std::string& err) {
    const size_t n = cells.size();
    std::vector<uint8_t> buf(17 + n * 13);
    std::memcpy(buf.data(), kMagic, 4);
    buf[4] = 3;
    storeU32(buf.data() + 5, uint32_t(n));
    storeU64(buf.data() + 9, totalNotes);
    uint8_t* posOut = buf.data() + 17;
    uint8_t* laneOut = posOut + n * 4;
    uint8_t* cntOut = laneOut + n;
    for (size_t i = 0; i < n; ++i) {
        storeU32(posOut + i * 4, uint32_t(cells[i].time));
        laneOut[i] = cells[i].lane;
        storeU64(cntOut + i * 8, cells[i].count);
    }
    return writeFileAtomic(path, buf.data(), buf.size(), err);
}

bool writeV4OrV5File(const std::string& path, const std::vector<Cell>& cells, uint64_t totalNotes, uint8_t version,
                     IutkRleMode rleMode, const std::string& embeddedMetaJson, bool compressMeta, std::string& err) {
    // Group sorted cells by identical timestamp.
    std::vector<uint32_t> deltas;
    std::vector<uint8_t> masks;
    std::vector<uint64_t> countsM1;

    const uint32_t cellCount = uint32_t(cells.size());
    uint32_t groupCount = 0;
    uint32_t firstTime = 0;

    if (!cells.empty()) {
        deltas.reserve(cells.size());
        masks.reserve(cells.size());
        countsM1.reserve(cells.size());

        size_t i = 0;
        uint32_t prevTimeU = 0;
        while (i < cells.size()) {
            const int32_t t = cells[i].time;
            const uint32_t timeU = uint32_t(t) ^ kTimeSignFlip;
            if (groupCount == 0) {
                firstTime = timeU;
            } else {
                deltas.push_back(timeU - prevTimeU - 1u);
            }
            prevTimeU = timeU;
            uint8_t mask = 0;
            while (i < cells.size() && cells[i].time == t) {
                mask |= uint8_t(1u << cells[i].lane);
                countsM1.push_back(cells[i].count - 1ull);
                ++i;
            }
            masks.push_back(mask);
            ++groupCount;
        }
    }

    // Encode streams D, M, C (and compute CRCs) in parallel when large.
    std::vector<uint8_t> bufD, bufM, bufC;
    uint32_t crcD = 0, crcM = 0, crcC = 0;
    if (groupCount >= 16384u) {
        auto futD = std::async(std::launch::async, [&] {
            auto b = encodeV4Stream(deltas, false, rleMode);
            const uint32_t c = crc32Bytes(b.data(), b.size());
            return std::make_pair(std::move(b), c);
        });
        auto futM = std::async(std::launch::async, [&] {
            auto b = encodeV4Stream(masks, true, rleMode);
            const uint32_t c = crc32Bytes(b.data(), b.size());
            return std::make_pair(std::move(b), c);
        });
        bufC = encodeV4Stream(countsM1, false, rleMode);
        crcC = crc32Bytes(bufC.data(), bufC.size());
        auto resM = futM.get();
        bufM = std::move(resM.first);
        crcM = resM.second;
        auto resD = futD.get();
        bufD = std::move(resD.first);
        crcD = resD.second;
    } else {
        bufD = encodeV4Stream(deltas, false, rleMode);
        bufM = encodeV4Stream(masks, true, rleMode);
        bufC = encodeV4Stream(countsM1, false, rleMode);
        crcD = crc32Bytes(bufD.data(), bufD.size());
        crcM = crc32Bytes(bufM.data(), bufM.size());
        crcC = crc32Bytes(bufC.data(), bufC.size());
    }

    if (bufD.size() > 0x7FFFFFFFull || bufM.size() > 0x7FFFFFFFull || bufC.size() > 0x7FFFFFFFull) {
        err = "encoded iutk v4 stream exceeds 2 GB limit";
        return false;
    }

    // Optional V5 embedded metadata payload.
    std::vector<uint8_t> metaPayload;
    uint32_t metaFlags = 0, metaRawLen = 0, metaCrc = 0;
    if (version == 5 && !embeddedMetaJson.empty()) {
        metaRawLen = uint32_t(embeddedMetaJson.size());
        if (compressMeta && deflateMeta(embeddedMetaJson, metaPayload) && metaPayload.size() < embeddedMetaJson.size()) {
            metaFlags = 1u;
        } else {
            metaFlags = 0u;
            metaPayload.assign(embeddedMetaJson.begin(), embeddedMetaJson.end());
        }
        metaCrc = crc32Bytes(metaPayload.data(), metaPayload.size());
    }

    std::vector<uint8_t> file;
    file.reserve(128 + bufD.size() + bufM.size() + bufC.size() + metaPayload.size());
    file.insert(file.end(), kMagic, kMagic + 4);
    file.push_back(version);
    appendVarint(file, cellCount);
    appendVarint(file, groupCount);
    appendVarint(file, totalNotes);
    appendVarint(file, firstTime);
    appendVarint(file, bufD.size());
    appendVarint(file, bufM.size());
    appendVarint(file, bufC.size());
    appendU32(file, crcD);
    appendU32(file, crcM);
    appendU32(file, crcC);
    if (version == 5) {
        appendVarint(file, metaFlags);
        appendVarint(file, metaRawLen);
        appendVarint(file, metaPayload.size());
        appendU32(file, metaCrc);
    }
    file.insert(file.end(), bufD.begin(), bufD.end());
    file.insert(file.end(), bufM.begin(), bufM.end());
    file.insert(file.end(), bufC.begin(), bufC.end());
    if (!metaPayload.empty()) file.insert(file.end(), metaPayload.begin(), metaPayload.end());

    return writeFileAtomic(path, file.data(), file.size(), err);
}

}  // namespace

bool writeIutkChartNotes(const std::string& binPath, const std::vector<IutkInputNote>& notes, const IutkMetadata& meta,
                         const std::vector<psych::ChartEvent>& events, const IutkWriteOptions& opt, std::string& err,
                         IutkWriteResult* result) {
    std::vector<Cell> cells;
    uint64_t totalNotes = 0;
    if (!buildCells(notes, opt.timeRound, cells, totalNotes, err)) return false;

    const fs::path bp = toPath(binPath);
    const fs::path dir = bp.parent_path();
    std::error_code ec;
    if (!dir.empty()) fs::create_directories(dir, ec);

    // Determine effective binary version:
    //   Embedded metadata requires V5; SeparateFile or None uses requested V2/V3/V4 (clamped to V4 if V5 was set).
    uint8_t ver = uint8_t(opt.version);
    std::string embeddedJson;
    if (opt.metaMode == IutkMetaMode::Embedded) {
        ver = 5;
        embeddedJson = buildIutkMetadataJson(meta, opt.includeSections, true);
    } else if (ver == 5) {
        ver = 4;
    }

    bool ok = false;
    if (ver == 2) ok = writeV2File(binPath, cells, totalNotes, err);
    else if (ver == 3) ok = writeV3File(binPath, cells, totalNotes, err);
    else ok = writeV4OrV5File(binPath, cells, totalNotes, ver, opt.rleMode, embeddedJson, opt.compressEmbeddedMeta, err);
    if (!ok) return false;

    IutkWriteResult res;
    res.binPath = binPath;
    res.binBytes = fileSize(binPath);
    res.cells = uint32_t(cells.size());
    res.totalNotes = totalNotes;

    if (opt.metaMode == IutkMetaMode::SeparateFile) {
        const std::string existingMeta = findSiblingI(dir, "metadata.json");
        const std::string metaPath = existingMeta.empty() ? pathStr(dir / "metadata.json") : existingMeta;
        const std::string metaJson = buildIutkMetadataJson(meta, opt.includeSections, opt.compactJson);
        if (!writeTextAtomic(metaPath, metaJson, err)) return false;
        res.metaPath = metaPath;
        res.metaBytes = metaJson.size();
    }

    if (opt.saveEventsJson) {
        const std::string existingEv = findSiblingI(dir, "events.json");
        const std::string evPath = existingEv.empty() ? pathStr(dir / "events.json") : existingEv;
        const std::string evJson = buildEventsJson(events, opt.compactJson);
        if (!writeTextAtomic(evPath, evJson, err)) return false;
        res.eventsPath = evPath;
        res.eventsBytes = evJson.size();
    }

    if (result) *result = std::move(res);
    return true;
}

bool writeIutkChart(const std::string& binPath, const Chart& chart, const IutkWriteOptions& opt, std::string& err,
                    IutkWriteResult* result) {
    std::vector<IutkInputNote> in;
    in.reserve(chart.notes.size());
    for (const Note& n : chart.notes) {
        in.push_back({n.time, n.side, n.lane, std::max<uint64_t>(1, n.stack)});
    }
    const IutkMetadata meta = metadataFromChart(chart);
    return writeIutkChartNotes(binPath, in, meta, chart.events, opt, err, result);
}

bool convertChartToIutk(const std::string& srcPath, const std::string& dstBinPath, const IutkWriteOptions& opt,
                        std::string& err, ConvertStats* stats, IutkWriteResult* result, const std::string& difficulty) {
    const auto t0 = std::chrono::steady_clock::now();
    Chart chart;
    if (!loadChartFile(srcPath, difficulty, chart, err)) return false;

    // Also pull sibling events.json if the source wasn't an iutk chart (loadIutkChart already reads it).
    if (!isIutkChart(srcPath)) {
        const std::string evPath = findSiblingI(toPath(srcPath).parent_path(), "events.json");
        if (!evPath.empty()) {
            std::string evText;
            if (readTextFile(evPath, evText)) {
                psych::parseEvents(evText, chart.events);
                std::stable_sort(chart.events.begin(), chart.events.end(),
                                 [](const psych::ChartEvent& a, const psych::ChartEvent& b) { return a.timeMs < b.timeMs; });
            }
        }
    }

    IutkWriteResult res;
    if (!writeIutkChart(dstBinPath, chart, opt, err, &res)) return false;
    if (stats) {
        stats->inBytes = fileSize(srcPath);
        stats->outBytes = res.binBytes;
        stats->notes = res.totalNotes;
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    if (result) *result = std::move(res);
    return true;
}

}  // namespace gh
