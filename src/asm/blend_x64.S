// Astralis Engine.
//
// The note renderer's inner loops in x86-64 assembly for GCC and Clang
// (Linux, Windows with MinGW, Android x86_64); blend_x64.asm is the same for MSVC.
//
//   void gh_blend_row_sse2(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256)
//   void gh_blend_row_sse4(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256)  [disabled unless GH_ASM_SSE4=1]
//   void gh_blend_row_avx2(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256)  [disabled unless GH_ASM_AVX2=1]
//
// Blends n premultiplied RGBA8 pixels of src over dst, src first scaled by
// alpha256 / 256, exactly as blendRowC in src/note_raster.cpp:
//   s = src * alpha256 >> 8
//   t = dst * (255 - s.a) + 128;   dst = s + ((t + (t >> 8)) >> 8)   (saturated)
// Only xmm0-xmm5 / ymm0-ymm5 and the argument registers are touched (all scratch
// in both System V AMD64 and Microsoft x64 ABIs).

#if defined(_WIN32)
#define DST rcx
#define SRC rdx
#define CNT r8
#define ALPHA r9d
#else
#define DST rdi
#define SRC rsi
#define CNT rdx
#define ALPHA ecx
#endif

#if defined(__APPLE__)
#define FN_SSE2 _gh_blend_row_sse2
#define FN_SSE4 _gh_blend_row_sse4
#define FN_AVX2 _gh_blend_row_avx2
#else
#define FN_SSE2 gh_blend_row_sse2
#define FN_SSE4 gh_blend_row_sse4
#define FN_AVX2 gh_blend_row_avx2
#endif

// One step on the pixels in xmm0 (source) and xmm1 (destination), as words;
// xmm3 = 255, xmm4 = alpha256, xmm5 = 0 in every word. Result in xmm1, as words.
#define BLEND_WORDS            \
    pmullw  xmm0, xmm4;        \
    psrlw   xmm0, 8;           \
    movdqa  xmm2, xmm0;        \
    pshuflw xmm2, xmm2, 0xFF;  \
    pshufhw xmm2, xmm2, 0xFF;  \
    pxor    xmm2, xmm3;        \
    pmullw  xmm1, xmm2;        \
    pcmpeqw xmm2, xmm2;        \
    psllw   xmm2, 15;          \
    psrlw   xmm2, 8;           \
    paddw   xmm1, xmm2;        \
    movdqa  xmm2, xmm1;        \
    psrlw   xmm2, 8;           \
    paddw   xmm1, xmm2;        \
    psrlw   xmm1, 8;           \
    paddw   xmm1, xmm0

    .intel_syntax noprefix
    .text
    .p2align 4
    .globl FN_SSE2
#if defined(__ELF__)
    .type FN_SSE2, @function
#endif
FN_SSE2:
    pxor        xmm5, xmm5          // 0
    movd        xmm4, ALPHA
    pshuflw     xmm4, xmm4, 0       // alpha256 in the four low words...
    punpcklqdq  xmm4, xmm4          // ...and in all eight
    pcmpeqw     xmm3, xmm3
    psrlw       xmm3, 8             // 255 in every word (255 - a == a ^ 255 for a <= 255)
    cmp         CNT, 2
    jb          2f
1:  // two pixels
    movq        xmm0, qword ptr [SRC]
    punpcklbw   xmm0, xmm5
    movq        xmm1, qword ptr [DST]
    punpcklbw   xmm1, xmm5
    BLEND_WORDS
    packuswb    xmm1, xmm1
    movq        qword ptr [DST], xmm1
    add         SRC, 8
    add         DST, 8
    sub         CNT, 2
    cmp         CNT, 2
    jae         1b
2:  // the last pixel when n is odd
    test        CNT, CNT
    jz          3f
    movd        xmm0, dword ptr [SRC]
    punpcklbw   xmm0, xmm5
    movd        xmm1, dword ptr [DST]
    punpcklbw   xmm1, xmm5
    BLEND_WORDS
    packuswb    xmm1, xmm1
    movd        dword ptr [DST], xmm1
3:
    ret
#if defined(__ELF__)
    .size FN_SSE2, . - FN_SSE2
#endif

#if defined(GH_ASM_SSE4) && GH_ASM_SSE4
    .p2align 4
    .globl FN_SSE4
#if defined(__ELF__)
    .type FN_SSE4, @function
#endif
FN_SSE4:
    movd        xmm4, ALPHA
    pshuflw     xmm4, xmm4, 0
    punpcklqdq  xmm4, xmm4
    pcmpeqw     xmm3, xmm3
    psrlw       xmm3, 8
    cmp         CNT, 2
    jb          2f
1:
    pmovzxbw    xmm0, qword ptr [SRC]
    pmovzxbw    xmm1, qword ptr [DST]
    BLEND_WORDS
    packuswb    xmm1, xmm1
    movq        qword ptr [DST], xmm1
    add         SRC, 8
    add         DST, 8
    sub         CNT, 2
    cmp         CNT, 2
    jae         1b
2:
    test        CNT, CNT
    jz          3f
    movd        xmm0, dword ptr [SRC]
    pmovzxbw    xmm0, xmm0
    movd        xmm1, dword ptr [DST]
    pmovzxbw    xmm1, xmm1
    BLEND_WORDS
    packuswb    xmm1, xmm1
    movd        dword ptr [DST], xmm1
3:
    ret
#if defined(__ELF__)
    .size FN_SSE4, . - FN_SSE4
#endif
#endif

#if defined(GH_ASM_AVX2) && GH_ASM_AVX2
    .p2align 4
    .globl FN_AVX2
#if defined(__ELF__)
    .type FN_AVX2, @function
#endif
FN_AVX2:
    vmovd       xmm4, ALPHA
    vpbroadcastw ymm4, xmm4         // alpha256 in all 16 words
    vpcmpeqw    ymm3, ymm3, ymm3
    vpsrlw      ymm3, ymm3, 8       // 255 in all 16 words
    vpcmpeqw    ymm5, ymm5, ymm5
    vpsllw      ymm5, ymm5, 15
    vpsrlw      ymm5, ymm5, 8       // 128 in all 16 words
    cmp         CNT, 4
    jb          2f
1:  // four pixels per iteration
    vpmovzxbw   ymm0, xmmword ptr [SRC]
    vpmovzxbw   ymm1, xmmword ptr [DST]
    vpmullw     ymm0, ymm0, ymm4
    vpsrlw      ymm0, ymm0, 8
    vpshuflw    ymm2, ymm0, 0xFF
    vpshufhw    ymm2, ymm2, 0xFF
    vpxor       ymm2, ymm2, ymm3
    vpmullw     ymm1, ymm1, ymm2
    vpaddw      ymm1, ymm1, ymm5
    vpsrlw      ymm2, ymm1, 8
    vpaddw      ymm1, ymm1, ymm2
    vpsrlw      ymm1, ymm1, 8
    vpaddw      ymm1, ymm1, ymm0
    vpackuswb   ymm1, ymm1, ymm1
    vpermq      ymm1, ymm1, 0xD8    // gather low 64 bits of each 128-bit lane into xmm1
    vmovdqu     xmmword ptr [DST], xmm1
    add         SRC, 16
    add         DST, 16
    sub         CNT, 4
    cmp         CNT, 4
    jae         1b
2:
    vzeroupper
    test        CNT, CNT
    jz          3f
    jmp         FN_SSE2
3:
    ret
#if defined(__ELF__)
    .size FN_AVX2, . - FN_AVX2
#endif
#endif

#if defined(__ELF__)
    .section .note.GNU-stack, "", @progbits
#endif
