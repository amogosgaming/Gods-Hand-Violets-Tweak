// Astralis Engine.
//
// Notes drawn on the processor (Options > Graphics > Note renderer): note
// pictures are blended into a picture of the screen here, a row at a time,
// by x86 assembly (x86 SSE2 enabled; x86-64 SSE4 and AVX2 disabled), ARM64
// NEON, or portable C++, and the result goes to the graphics card in one piece.
// Pixels are premultiplied RGBA8 (R G B A in memory). Kept apart from raylib
// so the tests can check it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gh::raster {

// The 3 x86 CPU assembly options for the ASM note renderer.
// SSE4 and AVX2 are disabled by default for broad CPU compatibility (e.g. Intel Core i5 3rd gen).
enum class CpuAsmOption : int {
    X86Sse2 = 0,
    X64Sse4 = 1,
    Avx2 = 2,
};

// Returns true when the given x86 CPU assembly option is enabled in this build and supported on this CPU.
bool cpuAsmEnabled(CpuAsmOption opt);
const char* cpuAsmLabel(CpuAsmOption opt);
bool cpuSupportsSse4();
bool cpuSupportsAvx2();

// Blends n pixels of src over dst, src first scaled by alpha256 / 256 (0..256):
//   s = src * alpha256 / 256;  dst = s + dst * (255 - s.a) / 255
// All versions give exactly the same pixels.
void blendRowC(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256);
void blendRowAsm(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256);  // active enabled ASM routine, or blendRowC
void blendRowAsmOption(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256, CpuAsmOption opt);
bool haveAsm();  // this build has an enabled assembly routine (x86 SSE2 or ARM64 NEON)
const char* asmName();  // "x86 SSE2", "ARM64 NEON" or "" (none)
// Compares the active assembly routine with the C++ one on random pixels; false
// when they differ (the game then uses the C++ one).
bool selfTest();

struct Sprite {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // premultiplied, w * h
    // Per row, the columns that are not fully transparent: [x0, x1) packed
    // as x0 | x1 << 16 (empty rows 0). Made by findSpans; blit skips the rest.
    std::vector<uint32_t> spans;
    int left = 0, top = 0, right = 0, bottom = 0;  // around the visible pixels (findSpans)
};
void findSpans(Sprite& s);

// The picture the notes are drawn into, frame after frame. Only the rows
// drawn last frame are cleared, and only the rows drawn now or last frame
// need to go to the graphics card again.
class Layer {
public:
    void resize(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    void begin();  // a new frame
    // s with its top-left corner at (x, y), clipped to the picture.
    void blit(const Sprite& s, int x, int y, float alpha, bool useAsm, CpuAsmOption opt = CpuAsmOption::X86Sse2);
    // What to upload now: [uploadLeft, uploadRight) x [uploadTop, uploadBottom),
    // what was drawn since begin() and what begin() cleared; empty when equal.
    int uploadTop() const;
    int uploadBottom() const;
    int uploadLeft() const;
    int uploadRight() const;
    const uint32_t* pixels() const { return px_.data(); }
    uint64_t blendedPixels() const { return blended_; }  // this frame

private:
    int w_ = 0, h_ = 0;
    std::vector<uint32_t> px_;
    int top_ = 0, bottom_ = 0, left_ = 0, right_ = 0;  // drawn this frame
    int prevTop_ = 0, prevBottom_ = 0, prevLeft_ = 0, prevRight_ = 0;  // drawn before begin() (cleared now)
    uint64_t blended_ = 0;
};

}  // namespace gh::raster
