// Checks the float16 vpkd3d128/vupkd3d128 codegen against PPC lane semantics.
// Build: clang++ -std=c++20 -march=x86-64-v3 -I generated/ppc -I tools/XenonRecomp/thirdparty/simde tests/float16_layout.cpp
#include "ppc_config.h"
#include "ppc_context.h"
#include <cstdio>
// Build a register from PPC big-endian bytes the way lvx does (full byte reversal).
static PPCVRegister FromPPC(const uint8_t (&b)[16]) { PPCVRegister r; for (int k = 0; k < 16; k++) r.u8[15 - k] = b[k]; return r; }
static float Lane(const PPCVRegister& r, int i) { return r.f32[3 - i]; } // PPC lane i
static uint32_t Word(const PPCVRegister& r, int i) { return r.u32[3 - i]; }
int main()
{
    int fails = 0;
    auto check = [&](const char* what, float got, float want) { if (got != want) { printf("FAIL %s: got %g want %g\n", what, got, want); fails++; } };
    // PPC halfwords 0..7; halves: 1.0=0x3C00, 2.0=0x4000, -0.5=0xB800, 65504=0x7BFF
    uint8_t b[16] = { 0x11,0x11, 0x22,0x22, 0x33,0x33, 0x44,0x44,  0x3C,0x00, 0x40,0x00, 0xB8,0x00, 0x7B,0xFF };
    PPCVRegister v1 = FromPPC(b), v0, vTemp;
    // vupkd3d128 float16_4 (as generated)
    simde_mm_store_ps(vTemp.f32, simde_mm_cvtph_ps(simde_mm_load_si128((simde__m128i*)v1.u8)));
    v0 = vTemp;
    check("f16_4 x", Lane(v0,0), 1.0f); check("f16_4 y", Lane(v0,1), 2.0f); check("f16_4 z", Lane(v0,2), -0.5f); check("f16_4 w", Lane(v0,3), 65504.0f);
    // vupkd3d128 float16_2: PPC halfwords 6,7 -> x,y; z=0, w=1
    simde_mm_store_ps(vTemp.f32, simde_mm_cvtph_ps(simde_mm_load_si128((simde__m128i*)v1.u8)));
    vTemp.f32[3] = vTemp.f32[1]; vTemp.f32[2] = vTemp.f32[0]; vTemp.f32[1] = 0.0f; vTemp.f32[0] = 1.0f;
    v0 = vTemp;
    check("f16_2 x", Lane(v0,0), -0.5f); check("f16_2 y", Lane(v0,1), 65504.0f); check("f16_2 z", Lane(v0,2), 0.0f); check("f16_2 w", Lane(v0,3), 1.0f);
    // vpkd3d128 float16_2, pack 1, shift 3 -> PPC word x = (h(x)<<16)|h(y), other words preserved
    PPCVRegister src; src.f32[3] = 1.0f; src.f32[2] = 2.0f; src.f32[1] = 9.0f; src.f32[0] = 9.0f;
    PPCVRegister dst = FromPPC(b);
    simde_mm_store_si128((simde__m128i*)vTemp.u8, simde_mm_cvtps_ph(simde_mm_load_ps(src.f32), 3));
    dst.u32[3] = (uint32_t(vTemp.u16[3]) << 16) | vTemp.u16[2];
    if (Word(dst,0) != 0x3C004000) { printf("FAIL pack x word %08X\n", Word(dst,0)); fails++; }
    if (Word(dst,3) != 0xB8007BFF) { printf("FAIL pack preserved w %08X\n", Word(dst,3)); fails++; }
    printf(fails ? "%d failures\n" : "all float16 checks passed\n", fails);
    return fails != 0;
}
