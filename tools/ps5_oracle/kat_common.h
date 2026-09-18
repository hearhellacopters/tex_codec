/*
 * kat_common.h - shared between tools/ps5_oracle (generator) and
 * tests/test_main.cpp (consumer): the deterministic surface filler, the
 * hash, and the list of public-API cases. Both sides must agree exactly.
 */
#ifndef TEXC_PS5_KAT_COMMON_H
#define TEXC_PS5_KAT_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "../../include/tex_codec.h"

static inline uint64_t kat_seed(size_t case_index)
{
    return 0x243F6A8885A308D3ull + 0x9E3779B97F4A7C15ull * (uint64_t)(case_index + 1);
}

/* xorshift64* byte stream. */
static inline void kat_fill(uint8_t *p, size_t n, uint64_t seed)
{
    uint64_t s = seed ? seed : 1;
    for (size_t i = 0; i < n; ++i) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        p[i] = (uint8_t)((s * 0x2545F4914F6CDD1Dull) >> 56);
    }
}

static inline uint64_t kat_fnv_init(void) { return 0xCBF29CE484222325ull; }
static inline uint64_t kat_fnv(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

struct kat_case {
    uint32_t tile_mode;
    texc_format format;
    const char *format_name;
    uint32_t w, h, mips, slices;
};

#define KC(tm, F, w, h, mips, slices) { tm, TEXC_FORMAT_##F, "TEXC_FORMAT_" #F, w, h, mips, slices }
static const kat_case k_kat_cases[] = {
    /* the G1T-relevant mode: STANDARD_4KB */
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC7,   256, 256, 9, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC7,   512, 128, 10, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC7,   128, 512, 10, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC1,   256, 256, 9, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC1,   64,  32,  7, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  RGBA8, 256, 256, 9, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  RGBA8, 64,  128, 8, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC7,   37,  23,  6, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC1,   100, 60,  7, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  RGBA8, 300, 70,  9, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC7,   64,  64,  7, 6),   /* cube map */
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC1,   32,  32,  1, 4),   /* array, no mips */
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC7,   1,   1,   1, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  BC7,   4,   4,   1, 1),
    KC(TEXC_PS5_TILE_STANDARD_4KB,  RGBA8, 5,   999, 10, 1),
    /* the other standard modes */
    KC(TEXC_PS5_TILE_STANDARD_64KB, BC7,   512, 512, 10, 1),
    KC(TEXC_PS5_TILE_STANDARD_64KB, BC1,   256, 128, 9, 1),
    KC(TEXC_PS5_TILE_STANDARD_64KB, RGBA8, 300, 70,  9, 2),
    KC(TEXC_PS5_TILE_STANDARD_64KB, BC7,   37,  23,  6, 1),
    KC(TEXC_PS5_TILE_STANDARD_256B, BC7,   256, 256, 9, 1),
    KC(TEXC_PS5_TILE_STANDARD_256B, BC1,   100, 60,  7, 3),
    KC(TEXC_PS5_TILE_STANDARD_256B, RGBA8, 64,  64,  7, 1),
};
#undef KC
static const size_t k_kat_case_count = sizeof(k_kat_cases) / sizeof(k_kat_cases[0]);

#endif /* TEXC_PS5_KAT_COMMON_H */
