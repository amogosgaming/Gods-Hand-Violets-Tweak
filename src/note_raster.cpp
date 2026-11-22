// Astralis Engine.

#include "note_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define GH_IS_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

// SSE4 and AVX2 ASM paths are intentionally disabled for compatibility.
#ifndef GH_ASM_SSE4
#define GH_ASM_SSE4 0
#endif
#ifndef GH_ASM_AVX2
#define GH_ASM_AVX2 0
#endif

#if GH_X86_ASM
// src/asm/blend_x64.S / blend_x64.asm (64-bit) or src/asm/blend_x86.S (32-bit).
extern "C" void gh_blend_row_sse2(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256);
#if GH_ASM_SSE4
extern "C" void gh_blend_row_sse4(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256);
#endif
#if GH_ASM_AVX2
extern "C" void gh_blend_row_avx2(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256);
#endif
#define GH_ASM_ROW gh_blend_row_sse2
#define GH_ASM_NAME "x86 SSE2"
#elif GH_ARM_ASM
// src/asm/blend_arm64.S (Android, Apple Silicon, ARM Linux).
extern "C" void gh_blend_row_neon(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256);
#define GH_ASM_ROW gh_blend_row_neon
#define GH_ASM_NAME "ARM64 NEON"
#endif

namespace gh::raster {

namespace {

struct CpuCaps {
    bool sse4 = false;
    bool avx2 = false;
};

CpuCaps detectCpuCaps() {
    CpuCaps caps;
#if defined(GH_IS_X86)
#if defined(_MSC_VER)
    int info[4] = {};
    __cpuid(info, 0);
    const int maxLeaf = info[0];
    if (maxLeaf >= 1) {
        __cpuid(info, 1);
        caps.sse4 = (info[2] & (1 << 19)) != 0 && (info[2] & (1 << 20)) != 0;  // SSE4.1 + SSE4.2
        const bool osxsave = (info[2] & (1 << 27)) != 0;
        const bool avx = (info[2] & (1 << 28)) != 0;
        if (osxsave && avx && maxLeaf >= 7) {
            const unsigned long long xcr0 = _xgetbv(0);
            if ((xcr0 & 0x6) == 0x6) {
                int ext[4] = {};
                __cpuidex(ext, 7, 0);
                caps.avx2 = (ext[1] & (1 << 5)) != 0;
            }
        }
    }
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(0, &a, &b, &c, &d)) {
        const unsigned maxLeaf = a;
        if (maxLeaf >= 1 && __get_cpuid(1, &a, &b, &c, &d)) {
            caps.sse4 = (c & (1u << 19)) != 0 && (c & (1u << 20)) != 0;
            const bool osxsave = (c & (1u << 27)) != 0;
            const bool avx = (c & (1u << 28)) != 0;
            if (osxsave && avx && maxLeaf >= 7) {
                unsigned eax = 0, edx = 0;
                __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
                if ((eax & 0x6u) == 0x6u) {
                    unsigned a7 = 0, b7 = 0, c7 = 0, d7 = 0;
                    __get_cpuid_count(7, 0, &a7, &b7, &c7, &d7);
                    caps.avx2 = (b7 & (1u << 5)) != 0;
                }
            }
        }
    }
#endif
#endif
    return caps;
}

const CpuCaps& cpuCaps() {
    static const CpuCaps c = detectCpuCaps();
    return c;
}

}  // namespace

bool cpuSupportsSse4() { return cpuCaps().sse4; }
bool cpuSupportsAvx2() { return cpuCaps().avx2; }

bool cpuAsmEnabled(CpuAsmOption opt) {
    switch (opt) {
        case CpuAsmOption::X86Sse2:
#if GH_X86_ASM
            return true;
#else
            return false;
#endif
        case CpuAsmOption::X64Sse4:
            // Intentionally disabled (requires GH_ASM_SSE4=1).
#if GH_X86_ASM && GH_ASM_SSE4 && (defined(__x86_64__) || defined(_M_X64))
            return cpuSupportsSse4();
#else
            return false;
#endif
        case CpuAsmOption::Avx2:
            // Intentionally disabled (requires GH_ASM_AVX2=1).
#if GH_X86_ASM && GH_ASM_AVX2 && (defined(__x86_64__) || defined(_M_X64))
            return cpuSupportsAvx2();
#else
            return false;
#endif
    }
    return false;
}

const char* cpuAsmLabel(CpuAsmOption opt) {
    switch (opt) {
        case CpuAsmOption::X86Sse2: return cpuAsmEnabled(opt) ? "x86 SSE2" : "x86 SSE2 (Unavailable)";
        case CpuAsmOption::X64Sse4: return cpuAsmEnabled(opt) ? "x86-64 SSE4" : "x86-64 SSE4 (Disabled)";
        case CpuAsmOption::Avx2: return cpuAsmEnabled(opt) ? "AVX2" : "AVX2 (Disabled)";
    }
    return "x86 SSE2";
}

void blendRowC(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256) {
    for (size_t i = 0; i < n; ++i) {
        const uint32_t s = src[i];
        uint32_t c[4];
        for (int k = 0; k < 4; ++k) c[k] = (((s >> (8 * k)) & 255u) * alpha256) >> 8;
        const uint32_t inv = 255u - c[3];
        const uint32_t d = dst[i];
        uint32_t out = 0;
        for (int k = 0; k < 4; ++k) {
            uint32_t t = ((d >> (8 * k)) & 255u) * inv + 128u;  // x / 255, rounded: (t + (t >> 8)) >> 8
            t = (t + (t >> 8)) >> 8;
            out |= std::min(255u, c[k] + t) << (8 * k);
        }
        dst[i] = out;
    }
}

void blendRowAsmOption(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256, CpuAsmOption opt) {
#if GH_X86_ASM
    if (opt == CpuAsmOption::X64Sse4 && cpuAsmEnabled(opt)) {
#if GH_ASM_SSE4 && (defined(__x86_64__) || defined(_M_X64))
        gh_blend_row_sse4(dst, src, n, alpha256);
        return;
#endif
    }
    if (opt == CpuAsmOption::Avx2 && cpuAsmEnabled(opt)) {
#if GH_ASM_AVX2 && (defined(__x86_64__) || defined(_M_X64))
        gh_blend_row_avx2(dst, src, n, alpha256);
        return;
#endif
    }
    // Default / fallback when an option is disabled: pure x86 SSE2.
    gh_blend_row_sse2(dst, src, n, alpha256);
#elif GH_ARM_ASM
    (void)opt;
    gh_blend_row_neon(dst, src, n, alpha256);
#else
    (void)opt;
    blendRowC(dst, src, n, alpha256);
#endif
}

void blendRowAsm(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256) {
    blendRowAsmOption(dst, src, n, alpha256, CpuAsmOption::X86Sse2);
}

bool haveAsm() {
#if defined(GH_ASM_ROW)
    return true;
#else
    return false;
#endif
}

const char* asmName() {
#if defined(GH_ASM_ROW)
    return GH_ASM_NAME;
#else
    return "";
#endif
}

bool selfTest() {
    if (!haveAsm()) return true;
    std::mt19937 rng(1234);
    std::vector<uint32_t> src(67), a(67), b(67);
    for (int round = 0; round < 64; ++round) {
        for (size_t i = 0; i < src.size(); ++i) {
            // Premultiplied: colour channels never above alpha.
            const uint32_t al = rng() & 255u;
            uint32_t p = al << 24;
            for (int k = 0; k < 3; ++k) p |= (al ? rng() % (al + 1) : 0u) << (8 * k);
            src[i] = p;
            a[i] = b[i] = rng();
        }
        const uint32_t alpha = round == 0 ? 256u : uint32_t(rng() % 257u);
        const size_t n = 1 + size_t(round) % 66;  // odd and even counts: the assembly's last pixel
        const size_t off = size_t(round) & 1;     // and pixels not on 8-byte boundaries
        blendRowC(a.data() + off, src.data(), n, alpha);
        blendRowAsm(b.data() + off, src.data(), n, alpha);
        if (std::memcmp(a.data(), b.data(), a.size() * 4) != 0) return false;
    }
    return true;
}

void findSpans(Sprite& s) {
    s.spans.assign(size_t(std::max(0, s.h)), 0u);
    s.left = s.w, s.top = s.h, s.right = 0, s.bottom = 0;
    for (int y = 0; y < s.h; ++y) {
        const uint32_t* row = &s.px[size_t(y) * size_t(s.w)];
        int x0 = 0, x1 = s.w;
        while (x0 < x1 && (row[x0] >> 24) == 0) ++x0;
        while (x1 > x0 && (row[x1 - 1] >> 24) == 0) --x1;
        s.spans[size_t(y)] = x0 < x1 ? uint32_t(x0) | uint32_t(x1) << 16 : 0u;
        if (x0 < x1) {
            s.left = std::min(s.left, x0), s.right = std::max(s.right, x1);
            s.top = std::min(s.top, y), s.bottom = y + 1;
        }
    }
    if (s.right <= s.left) s.left = s.top = s.right = s.bottom = 0;
}

void Layer::resize(int w, int h) {
    if (w == w_ && h == h_) return;
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    px_.assign(size_t(w_) * size_t(h_), 0u);  // the caller makes a new, empty texture too
    top_ = prevTop_ = h_;
    bottom_ = prevBottom_ = 0;
    left_ = prevLeft_ = w_;
    right_ = prevRight_ = 0;
}

void Layer::begin() {
    // Clear what the last frame drew; those rows go to the graphics card again, empty.
    if (bottom_ > top_ && right_ > left_)
        for (int y = top_; y < bottom_; ++y) std::memset(&px_[size_t(y) * w_ + left_], 0, size_t(right_ - left_) * 4);
    prevTop_ = top_;
    prevBottom_ = bottom_;
    prevLeft_ = left_;
    prevRight_ = right_;
    top_ = h_;
    bottom_ = 0;
    left_ = w_;
    right_ = 0;
    blended_ = 0;
}

void Layer::blit(const Sprite& s, int x, int y, float alpha, bool useAsm, CpuAsmOption opt) {
    if (s.w <= 0 || s.h <= 0 || !(alpha > 0.0f)) return;
    const uint32_t a256 = uint32_t(std::lround(std::min(alpha, 1.0f) * 256.0f));
    if (a256 == 0) return;
    const int x0 = std::max(0, x), x1 = std::min(w_, x + s.w);
    const int y0 = std::max(0, y), y1 = std::min(h_, y + s.h);
    if (x0 >= x1 || y0 >= y1) return;
    const bool spans = s.spans.size() == size_t(s.h);
    int drawnTop = y1, drawnBottom = y0;
    for (int yy = y0; yy < y1; ++yy) {
        int a = x0, b = x1;  // this row's columns, in the layer
        if (spans) {
            const uint32_t sp = s.spans[size_t(yy - y)];
            if (!sp) continue;  // fully transparent row
            a = std::max(a, x + int(sp & 0xFFFFu));
            b = std::min(b, x + int(sp >> 16));
            if (a >= b) continue;
        }
        uint32_t* dstRow = &px_[size_t(yy) * w_ + a];
        const uint32_t* srcRow = &s.px[size_t(yy - y) * s.w + size_t(a - x)];
        if (useAsm) blendRowAsmOption(dstRow, srcRow, size_t(b - a), a256, opt);
        else blendRowC(dstRow, srcRow, size_t(b - a), a256);
        blended_ += uint64_t(b - a);
        drawnTop = std::min(drawnTop, yy);
        drawnBottom = yy + 1;
    }
    if (drawnTop >= drawnBottom) return;
    top_ = std::min(top_, drawnTop);
    bottom_ = std::max(bottom_, drawnBottom);
    left_ = std::min(left_, x0);
    right_ = std::max(right_, x1);
}

int Layer::uploadTop() const {
    const bool now = bottom_ > top_, before = prevBottom_ > prevTop_;
    if (now && before) return std::min(top_, prevTop_);
    return now ? top_ : (before ? prevTop_ : 0);
}

int Layer::uploadBottom() const {
    const bool now = bottom_ > top_, before = prevBottom_ > prevTop_;
    if (now && before) return std::max(bottom_, prevBottom_);
    return now ? bottom_ : (before ? prevBottom_ : 0);
}

int Layer::uploadLeft() const {
    const bool now = bottom_ > top_, before = prevBottom_ > prevTop_;
    if (now && before) return std::min(left_, prevLeft_);
    return now ? left_ : (before ? prevLeft_ : 0);
}

int Layer::uploadRight() const {
    const bool now = bottom_ > top_, before = prevBottom_ > prevTop_;
    if (now && before) return std::max(right_, prevRight_);
    return now ? right_ : (before ? prevRight_ : 0);
}

}  // namespace gh::raster
