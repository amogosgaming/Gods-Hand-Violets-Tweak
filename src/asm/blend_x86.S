// Astralis Engine.
//
// The note renderer's inner loop in 32-bit x86 assembly (pure SSE2), for GCC,
// Clang and MinGW-w64 i686 (Windows Vista/7+ 32-bit and 32-bit x86 Linux).
//
//   void gh_blend_row_sse2(uint32_t* dst, const uint32_t* src, size_t n, uint32_t alpha256)
//   cdecl stack on entry:
//     [esp + 4]  = dst
//     [esp + 8]  = src
//     [esp + 12] = n
//     [esp + 16] = alpha256
//
// Pure SSE2 only (no SSE3/SSSE3/SSE4/AVX). Touches only eax, ecx, edx and
// xmm0-xmm5 (all caller-saved in cdecl), never pushes/aligns the stack.

#if defined(_WIN32) || defined(__APPLE__)
#define FUNC _gh_blend_row_sse2
#else
#define FUNC gh_blend_row_sse2
#endif

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
    .globl FUNC
#if defined(__ELF__)
    .type FUNC, @function
#endif
FUNC:
    mov         eax, dword ptr [esp + 4]   // dst
    mov         edx, dword ptr [esp + 8]   // src
    mov         ecx, dword ptr [esp + 12]  // n
    movd        xmm4, dword ptr [esp + 16] // alpha256
    pxor        xmm5, xmm5                 // 0
    pshuflw     xmm4, xmm4, 0              // alpha256 in the four low words...
    punpcklqdq  xmm4, xmm4                 // ...and in all eight
    pcmpeqw     xmm3, xmm3
    psrlw       xmm3, 8                    // 255 in every word
    cmp         ecx, 2
    jb          2f
1:  // two pixels
    movq        xmm0, qword ptr [edx]
    punpcklbw   xmm0, xmm5
    movq        xmm1, qword ptr [eax]
    punpcklbw   xmm1, xmm5
    BLEND_WORDS
    packuswb    xmm1, xmm1
    movq        qword ptr [eax], xmm1
    add         edx, 8
    add         eax, 8
    sub         ecx, 2
    cmp         ecx, 2
    jae         1b
2:  // the last pixel when n is odd
    test        ecx, ecx
    jz          3f
    movd        xmm0, dword ptr [edx]
    punpcklbw   xmm0, xmm5
    movd        xmm1, dword ptr [eax]
    punpcklbw   xmm1, xmm5
    BLEND_WORDS
    packuswb    xmm1, xmm1
    movd        dword ptr [eax], xmm1
3:
    ret
#if defined(__ELF__)
    .size FUNC, . - FUNC
    .section .note.GNU-stack, "", @progbits
#endif
