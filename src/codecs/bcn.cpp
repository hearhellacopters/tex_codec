/*
 * bcn.cpp - BC/DXT family codec module (BC1-BC7).
 *
 * Implements the codec_common.h contract for:
 *   decode: BC1, BC2, BC3, BC4(+snorm), BC5(+snorm), BC6H UF16/SF16, BC7
 *   encode: BC1/2/3 (PCA endpoint fit + index refinement, punch-through
 *           alpha for BC1), BC4/5 (min/max, 8-value indices),
 *           BC7 mode 6, BC6H mode 11.
 *
 * Bit layouts, unquantization, partition/anchor tables and interpolation
 * weights follow the D3D11 functional spec (BC6H/BC7) and the
 * ARB_texture_compression_bptc tables.
 */

#include "codec_common.h"
#include <math.h>

namespace texc {
namespace {

/* ------------------------------------------------------------ bit I/O -- */

static inline uint64_t load64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);              /* little-endian assumed */
    return v;
}

struct BitR {                       /* 128-bit LSB-first reader */
    uint64_t lo, hi;
    uint32_t get(int n) {           /* n in [1,32] */
        uint32_t mask = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
        uint32_t v = (uint32_t)lo & mask;
        lo = (lo >> n) | (hi << (64 - n));
        hi >>= n;
        return v;
    }
    uint32_t get_rev(int n) {       /* read n bits, reverse bit order */
        uint32_t bits = get(n), r = 0;
        while (n--) { r = (r << 1) | (bits & 1u); bits >>= 1; }
        return r;
    }
};

struct BitW {                       /* LSB-first writer over a 16B block */
    uint8_t *p;
    int pos;
    void put(uint32_t v, int n) {
        for (int i = 0; i < n; i++) {
            if (v & (1u << i)) p[pos >> 3] |= (uint8_t)(1u << (pos & 7));
            pos++;
        }
    }
};

static inline void set_px(uint8_t *d, int r, int g, int b, int a) {
    d[0] = clamp_u8(r); d[1] = clamp_u8(g); d[2] = clamp_u8(b); d[3] = clamp_u8(a);
}

static inline int div_round(int num, int den) {
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

/* ------------------------------------------- BC1 colour block (+BC2/3) -- */

/* Build the 4-entry palette for a BC1 colour block. `opaque_mode` is set
 * for the colour halves of BC2/BC3 (always 4-colour interpolation). */
static void bc1_palette(uint16_t c0, uint16_t c1, int opaque_mode,
                        uint8_t pal[4][4]) {
    int r0 = expand5(c0 >> 11), g0 = expand6((c0 >> 5) & 63), b0 = expand5(c0 & 31);
    int r1 = expand5(c1 >> 11), g1 = expand6((c1 >> 5) & 63), b1 = expand5(c1 & 31);
    set_px(pal[0], r0, g0, b0, 255);
    set_px(pal[1], r1, g1, b1, 255);
    if (c0 > c1 || opaque_mode) {
        set_px(pal[2], (2 * r0 + r1 + 1) / 3, (2 * g0 + g1 + 1) / 3, (2 * b0 + b1 + 1) / 3, 255);
        set_px(pal[3], (r0 + 2 * r1 + 1) / 3, (g0 + 2 * g1 + 1) / 3, (b0 + 2 * b1 + 1) / 3, 255);
    } else {
        set_px(pal[2], (r0 + r1 + 1) >> 1, (g0 + g1 + 1) >> 1, (b0 + b1 + 1) >> 1, 255);
        set_px(pal[3], 0, 0, 0, 0);
    }
}

static void decode_bc1_block(const uint8_t *s, uint8_t out[64], int opaque_mode) {
    uint16_t c0 = (uint16_t)(s[0] | (s[1] << 8));
    uint16_t c1 = (uint16_t)(s[2] | (s[3] << 8));
    uint8_t pal[4][4];
    bc1_palette(c0, c1, opaque_mode, pal);
    uint32_t idx = (uint32_t)s[4] | ((uint32_t)s[5] << 8) |
                   ((uint32_t)s[6] << 16) | ((uint32_t)s[7] << 24);
    for (int p = 0; p < 16; p++) {
        memcpy(out + p * 4, pal[idx & 3], 4);
        idx >>= 2;
    }
}

/* ------------------------------------------------ BC4-style 8B channel -- */

/* Palette for a BC4/BC3-alpha channel block. Values are 0..255 (unsigned)
 * or -127..127 (signed). */
static void bc4_palette(int a0, int a1, int sgn, int pal[8]) {
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; i++)
            pal[i + 1] = div_round((7 - i) * a0 + i * a1, 7);
    } else {
        for (int i = 1; i <= 4; i++)
            pal[i + 1] = div_round((5 - i) * a0 + i * a1, 5);
        pal[6] = sgn ? -127 : 0;
        pal[7] = sgn ? 127 : 255;
    }
}

/* Decode one 8-byte BC4 channel block into 16 raw values. */
static void decode_bc4_values(const uint8_t *s, int sgn, int out[16]) {
    int a0, a1;
    if (sgn) {
        a0 = (int8_t)s[0]; a1 = (int8_t)s[1];
        if (a0 < -127) a0 = -127;   /* -128 clamps to -127 per spec */
        if (a1 < -127) a1 = -127;
    } else {
        a0 = s[0]; a1 = s[1];
    }
    int pal[8];
    bc4_palette(a0, a1, sgn, pal);
    uint64_t idx = load64(s) >> 16;
    for (int p = 0; p < 16; p++) {
        out[p] = pal[idx & 7];
        idx >>= 3;
    }
}

/* SNORM value in [-127,127] -> UNORM8 via round(v*127.5 + 127.5). */
static uint8_t snorm_to_u8(int v) {
    float f = (float)v / 127.0f;
    if (f < -1.0f) f = -1.0f;
    if (f > 1.0f) f = 1.0f;
    return clamp_u8((int)(f * 127.5f + 127.5f + 0.5f));
}

/* UNORM8 -> SNORM code in [-127,127] (inverse of the mapping above). */
static int u8_to_snorm(uint8_t u) {
    float f = ((float)u - 127.5f) / 127.5f;
    int v = (int)floorf(f * 127.0f + 0.5f);
    if (v < -127) v = -127;
    if (v > 127) v = 127;
    return v;
}

/* ------------------------------------------------------- BC7 tables ---- */
/* Partition tables (2 bits/pixel packed LSB-first, pixel 0 = top-left) and
 * fix-up (anchor) index tables, per ARB_texture_compression_bptc.
 * BC6H two-region partitions are the first 32 entries of kP2/kA2. */

static const uint16_t kP2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
    0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
    0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A,
    0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
    0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};
static const uint8_t kA2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};
static const uint32_t kP3[64] = {
    0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050,
    0x5555A0A0, 0x5A5A5050, 0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090,
    0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250, 0xA5945040, 0x0A425054,
    0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
    0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414,
    0x50A4A450, 0x6A5A0200, 0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424,
    0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50, 0x500AA550, 0xAAAA4444,
    0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
    0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580,
    0xAA141414, 0x96960000, 0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000,
    0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254,
};
static const uint8_t kA3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};
static const uint8_t kA3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};

/* Index interpolation weight tables (spec-exact). */
static const int kW2[4]  = { 0, 21, 43, 64 };
static const int kW3[8]  = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const int kW4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

static inline int interp64(int a, int b, int w) {
    return (a * (64 - w) + b * w + 32) >> 6;
}

/* -------------------------------------------------------- BC7 decode --- */

static void decode_bc7_block(const uint8_t *s, uint8_t out[64]) {
    static const uint8_t kPartBits[8] = { 4, 6, 6, 6, 0, 0, 0, 6 };
    static const uint8_t kNumSub[8]   = { 3, 2, 3, 2, 1, 1, 1, 2 };
    static const uint8_t kCB[8]       = { 4, 6, 5, 7, 5, 7, 7, 5 };   /* colour bits */
    static const uint8_t kAB[8]       = { 0, 0, 0, 0, 6, 8, 7, 5 };   /* alpha bits  */
    static const uint8_t kIB1[8]      = { 3, 3, 2, 2, 2, 2, 4, 2 };
    static const uint8_t kIB2[8]      = { 0, 0, 0, 0, 3, 2, 0, 0 };
    static const uint8_t kHasPBits    = 0xCB;                          /* modes 0,1,3,6,7 */

    BitR br = { load64(s), load64(s + 8) };

    int mode = 0;
    while (mode < 8 && br.get(1) == 0) mode++;
    if (mode >= 8) {                       /* reserved: transparent black */
        memset(out, 0, 64);
        return;
    }

    int nsub = kNumSub[mode];
    int partition = 0, rotation = 0, isb = 0;
    if (kPartBits[mode]) partition = (int)br.get(kPartBits[mode]);
    if (mode == 4 || mode == 5) {
        rotation = (int)br.get(2);
        if (mode == 4) isb = (int)br.get(1);
    }

    int nep = nsub * 2;
    int ep[6][4];
    for (int c = 0; c < 3; c++)
        for (int e = 0; e < nep; e++)
            ep[e][c] = (int)br.get(kCB[mode]);
    if (kAB[mode]) {
        for (int e = 0; e < nep; e++)
            ep[e][3] = (int)br.get(kAB[mode]);
    } else {
        for (int e = 0; e < nep; e++)
            ep[e][3] = 0;
    }

    int haspb = (kHasPBits >> mode) & 1;
    if (haspb) {
        for (int e = 0; e < nep; e++)
            for (int c = 0; c < 4; c++)
                ep[e][c] <<= 1;
        if (mode == 1) {                    /* shared P-bit per subset */
            int p0 = (int)br.get(1), p1 = (int)br.get(1);
            for (int c = 0; c < 3; c++) {
                ep[0][c] |= p0; ep[1][c] |= p0;
                ep[2][c] |= p1; ep[3][c] |= p1;
            }
        } else {                            /* unique P-bit per endpoint */
            for (int e = 0; e < nep; e++) {
                int pb = (int)br.get(1);
                for (int c = 0; c < 4; c++)
                    ep[e][c] |= pb;
            }
        }
    }

    /* Dequantize endpoints to 8 bits (MSB replication). */
    for (int e = 0; e < nep; e++) {
        int cb = kCB[mode] + haspb;
        for (int c = 0; c < 3; c++) {
            ep[e][c] <<= (8 - cb);
            ep[e][c] |= ep[e][c] >> cb;
        }
        if (kAB[mode]) {
            int ab = kAB[mode] + haspb;
            ep[e][3] <<= (8 - ab);
            ep[e][3] |= ep[e][3] >> ab;
        } else {
            ep[e][3] = 0xFF;
        }
    }

    int ib1 = kIB1[mode], ib2 = kIB2[mode];
    const int *w1 = (ib1 == 2) ? kW2 : (ib1 == 3) ? kW3 : kW4;
    const int *w2 = (ib2 == 2) ? kW2 : kW3;

    /* Subset id + anchor flag per pixel. */
    int subset[16], anchor[16];
    for (int p = 0; p < 16; p++) {
        if (nsub == 1) {
            subset[p] = 0;
            anchor[p] = (p == 0);
        } else if (nsub == 2) {
            subset[p] = (kP2[partition] >> p) & 1;
            anchor[p] = (p == 0) || (p == kA2[partition]);
        } else {
            subset[p] = (int)((kP3[partition] >> (2 * p)) & 3);
            anchor[p] = (p == 0) || (p == kA3a[partition]) || (p == kA3b[partition]);
        }
    }

    /* Pass 1: primary indices. */
    int idx1[16];
    for (int p = 0; p < 16; p++)
        idx1[p] = (int)br.get(anchor[p] ? ib1 - 1 : ib1);

    /* Pass 2: secondary indices (if any) + interpolation. */
    for (int p = 0; p < 16; p++) {
        int e0 = subset[p] * 2, e1 = e0 + 1;
        int r, g, b, a;
        if (!ib2) {
            int w = w1[idx1[p]];
            r = interp64(ep[e0][0], ep[e1][0], w);
            g = interp64(ep[e0][1], ep[e1][1], w);
            b = interp64(ep[e0][2], ep[e1][2], w);
            a = interp64(ep[e0][3], ep[e1][3], w);
        } else {
            int i2 = (int)br.get(anchor[p] ? ib2 - 1 : ib2);
            int wc = isb ? w2[i2] : w1[idx1[p]];
            int wa = isb ? w1[idx1[p]] : w2[i2];
            r = interp64(ep[e0][0], ep[e1][0], wc);
            g = interp64(ep[e0][1], ep[e1][1], wc);
            b = interp64(ep[e0][2], ep[e1][2], wc);
            a = interp64(ep[e0][3], ep[e1][3], wa);
        }
        int t;
        switch (rotation) {
            case 1: t = a; a = r; r = t; break;
            case 2: t = a; a = g; g = t; break;
            case 3: t = a; a = b; b = t; break;
            default: break;
        }
        set_px(out + p * 4, r, g, b, a);
    }
}

/* -------------------------------------------------------- BC6H decode -- */
/* Per-mode field layout tables, transcribed from the D3D11 functional
 * spec ("Decoding the BC6H format"). Each op moves `cnt` bits from the
 * stream into endpoint field `tgt` at bit position `shift`.
 * tgt: 0-3 = r[w,x,y,z], 4-7 = g[w,x,y,z], 8-11 = b[w,x,y,z]. */

#define B6(t, s, c)  (uint16_t)(((t) << 8) | ((s) << 4) | (c))
#define B6R(t, s, c) (uint16_t)(0x8000u | ((t) << 8) | ((s) << 4) | (c))

static const uint16_t kB6M0[] = {  /* mode bits 00: 10.555, transformed, 2 regions */
    B6(6,4,1), B6(10,4,1), B6(11,4,1), B6(0,0,10), B6(4,0,10), B6(8,0,10),
    B6(1,0,5), B6(7,4,1), B6(6,0,4), B6(5,0,5), B6(11,0,1), B6(7,0,4),
    B6(9,0,5), B6(11,1,1), B6(10,0,4), B6(2,0,5), B6(11,2,1), B6(3,0,5),
    B6(11,3,1), 0
};
static const uint16_t kB6M1[] = {  /* 01: 7.666 */
    B6(6,5,1), B6(7,4,1), B6(7,5,1), B6(0,0,7), B6(11,0,1), B6(11,1,1),
    B6(10,4,1), B6(4,0,7), B6(10,5,1), B6(11,2,1), B6(6,4,1), B6(8,0,7),
    B6(11,3,1), B6(11,5,1), B6(11,4,1), B6(1,0,6), B6(6,0,4), B6(5,0,6),
    B6(7,0,4), B6(9,0,6), B6(10,0,4), B6(2,0,6), B6(3,0,6), 0
};
static const uint16_t kB6M2[] = {  /* 00010: 11.555 (5,4,4) */
    B6(0,0,10), B6(4,0,10), B6(8,0,10), B6(1,0,5), B6(0,10,1), B6(6,0,4),
    B6(5,0,4), B6(4,10,1), B6(11,0,1), B6(7,0,4), B6(9,0,4), B6(8,10,1),
    B6(11,1,1), B6(10,0,4), B6(2,0,5), B6(11,2,1), B6(3,0,5), B6(11,3,1), 0
};
static const uint16_t kB6M3[] = {  /* 00110: 11 (4,5,4) */
    B6(0,0,10), B6(4,0,10), B6(8,0,10), B6(1,0,4), B6(0,10,1), B6(7,4,1),
    B6(6,0,4), B6(5,0,5), B6(4,10,1), B6(7,0,4), B6(9,0,4), B6(8,10,1),
    B6(11,1,1), B6(10,0,4), B6(2,0,4), B6(11,0,1), B6(11,2,1), B6(3,0,4),
    B6(6,4,1), B6(11,3,1), 0
};
static const uint16_t kB6M4[] = {  /* 01010: 11 (4,4,5) */
    B6(0,0,10), B6(4,0,10), B6(8,0,10), B6(1,0,4), B6(0,10,1), B6(10,4,1),
    B6(6,0,4), B6(5,0,4), B6(4,10,1), B6(11,0,1), B6(7,0,4), B6(9,0,5),
    B6(8,10,1), B6(10,0,4), B6(2,0,4), B6(11,1,1), B6(11,2,1), B6(3,0,4),
    B6(11,4,1), B6(11,3,1), 0
};
static const uint16_t kB6M5[] = {  /* 01110: 9.555 */
    B6(0,0,9), B6(10,4,1), B6(4,0,9), B6(6,4,1), B6(8,0,9), B6(11,4,1),
    B6(1,0,5), B6(7,4,1), B6(6,0,4), B6(5,0,5), B6(11,0,1), B6(7,0,4),
    B6(9,0,5), B6(11,1,1), B6(10,0,4), B6(2,0,5), B6(11,2,1), B6(3,0,5),
    B6(11,3,1), 0
};
static const uint16_t kB6M6[] = {  /* 10010: 8 (6,5,5) */
    B6(0,0,8), B6(7,4,1), B6(10,4,1), B6(4,0,8), B6(11,2,1), B6(6,4,1),
    B6(8,0,8), B6(11,3,1), B6(11,4,1), B6(1,0,6), B6(6,0,4), B6(5,0,5),
    B6(11,0,1), B6(7,0,4), B6(9,0,5), B6(11,1,1), B6(10,0,4), B6(2,0,6),
    B6(3,0,6), 0
};
static const uint16_t kB6M7[] = {  /* 10110: 8 (5,6,5) */
    B6(0,0,8), B6(11,0,1), B6(10,4,1), B6(4,0,8), B6(6,5,1), B6(6,4,1),
    B6(8,0,8), B6(7,5,1), B6(11,4,1), B6(1,0,5), B6(7,4,1), B6(6,0,4),
    B6(5,0,6), B6(7,0,4), B6(9,0,5), B6(11,1,1), B6(10,0,4), B6(2,0,5),
    B6(11,2,1), B6(3,0,5), B6(11,3,1), 0
};
static const uint16_t kB6M8[] = {  /* 11010: 8 (5,5,6) */
    B6(0,0,8), B6(11,1,1), B6(10,4,1), B6(4,0,8), B6(10,5,1), B6(6,4,1),
    B6(8,0,8), B6(11,5,1), B6(11,4,1), B6(1,0,5), B6(7,4,1), B6(6,0,4),
    B6(5,0,5), B6(11,0,1), B6(7,0,4), B6(9,0,6), B6(10,0,4), B6(2,0,5),
    B6(11,2,1), B6(3,0,5), B6(11,3,1), 0
};
static const uint16_t kB6M9[] = {  /* 11110: 6666, not transformed */
    B6(0,0,6), B6(7,4,1), B6(11,0,1), B6(11,1,1), B6(10,4,1), B6(4,0,6),
    B6(6,5,1), B6(10,5,1), B6(11,2,1), B6(6,4,1), B6(8,0,6), B6(7,5,1),
    B6(11,3,1), B6(11,5,1), B6(11,4,1), B6(1,0,6), B6(6,0,4), B6(5,0,6),
    B6(7,0,4), B6(9,0,6), B6(10,0,4), B6(2,0,6), B6(3,0,6), 0
};
static const uint16_t kB6M10[] = { /* 00011: 10.10, one region, no delta */
    B6(0,0,10), B6(4,0,10), B6(8,0,10), B6(1,0,10), B6(5,0,10), B6(9,0,10), 0
};
static const uint16_t kB6M11[] = { /* 00111: 11.9 */
    B6(0,0,10), B6(4,0,10), B6(8,0,10), B6(1,0,9), B6(0,10,1), B6(5,0,9),
    B6(4,10,1), B6(9,0,9), B6(8,10,1), 0
};
static const uint16_t kB6M12[] = { /* 01011: 12.8 */
    B6(0,0,10), B6(4,0,10), B6(8,0,10), B6(1,0,8), B6R(0,10,2), B6(5,0,8),
    B6R(4,10,2), B6(9,0,8), B6R(8,10,2), 0
};
static const uint16_t kB6M13[] = { /* 01111: 16.4 */
    B6(0,0,10), B6(4,0,10), B6(8,0,10), B6(1,0,4), B6R(0,10,6), B6(5,0,4),
    B6R(4,10,6), B6(9,0,4), B6R(8,10,6), 0
};

struct B6Mode {
    const uint16_t *ops;
    uint8_t epb;            /* endpoint (W) precision in bits            */
    uint8_t db[3];          /* delta precision per channel (R,G,B)       */
    uint8_t transformed;    /* delta-compressed endpoints                */
    uint8_t partitioned;    /* two regions + 5-bit partition             */
};

static const B6Mode kB6Modes[14] = {
    { kB6M0,  10, { 5,  5,  5  }, 1, 1 },
    { kB6M1,  7,  { 6,  6,  6  }, 1, 1 },
    { kB6M2,  11, { 5,  4,  4  }, 1, 1 },
    { kB6M3,  11, { 4,  5,  4  }, 1, 1 },
    { kB6M4,  11, { 4,  4,  5  }, 1, 1 },
    { kB6M5,  9,  { 5,  5,  5  }, 1, 1 },
    { kB6M6,  8,  { 6,  5,  5  }, 1, 1 },
    { kB6M7,  8,  { 5,  6,  5  }, 1, 1 },
    { kB6M8,  8,  { 5,  5,  6  }, 1, 1 },
    { kB6M9,  6,  { 6,  6,  6  }, 0, 1 },
    { kB6M10, 10, { 10, 10, 10 }, 0, 0 },
    { kB6M11, 11, { 9,  9,  9  }, 1, 0 },
    { kB6M12, 12, { 8,  8,  8  }, 1, 0 },
    { kB6M13, 16, { 4,  4,  4  }, 1, 0 },
};

/* Map the raw 2/5-bit mode value to a kB6Modes index (-1 = reserved). */
static int b6_mode_index(int mv) {
    switch (mv) {
        case 0x00: return 0;  case 0x01: return 1;  case 0x02: return 2;
        case 0x06: return 3;  case 0x0A: return 4;  case 0x0E: return 5;
        case 0x12: return 6;  case 0x16: return 7;  case 0x1A: return 8;
        case 0x1E: return 9;  case 0x03: return 10; case 0x07: return 11;
        case 0x0B: return 12; case 0x0F: return 13;
        default:   return -1;
    }
}

static inline int b6_sext(int v, int bits) {
    return (v << (32 - bits)) >> (32 - bits);
}

/* Spec unquantization to a 17-bit signed intermediate. */
static int b6_unquantize(int v, int bits, int sgn) {
    int unq, s = 0;
    if (!sgn) {
        if (bits >= 15)                 unq = v;
        else if (v == 0)                unq = 0;
        else if (v == (1 << bits) - 1)  unq = 0xFFFF;
        else                            unq = ((v << 16) + 0x8000) >> bits;
    } else {
        if (bits >= 16)                 unq = v;
        else {
            if (v < 0) { s = 1; v = -v; }
            if (v == 0)                          unq = 0;
            else if (v >= (1 << (bits - 1)) - 1) unq = 0x7FFF;
            else                                 unq = ((v << 15) + 0x4000) >> (bits - 1);
            if (s) unq = -unq;
        }
    }
    return unq;
}

/* Final scale to a half-float bit pattern (unsigned: *31/64; signed:
 * *31/32 with sign-magnitude packing). */
static uint16_t b6_finish_unquantize(int v, int sgn) {
    if (!sgn)
        return (uint16_t)((v * 31) >> 6);
    v = (v < 0) ? -(((-v) * 31) >> 5) : (v * 31) >> 5;
    int s = 0;
    if (v < 0) { s = 0x8000; v = -v; }
    return (uint16_t)(s | v);
}

/* Decode one BC6H block into 16 RGB half-float triples. */
static void decode_bc6h_block(const uint8_t *s, int sgn, uint16_t outh[48]) {
    BitR br = { load64(s), load64(s + 8) };

    int mv = (int)br.get(2);
    if (mv > 1) mv |= (int)br.get(3) << 2;
    int mi = b6_mode_index(mv);
    if (mi < 0) {                       /* reserved mode: zero all channels */
        memset(outh, 0, 48 * sizeof(uint16_t));
        return;
    }
    const B6Mode &m = kB6Modes[mi];

    int r[4] = { 0, 0, 0, 0 }, g[4] = { 0, 0, 0, 0 }, b[4] = { 0, 0, 0, 0 };
    int *field[3] = { r, g, b };
    for (const uint16_t *op = m.ops; *op; op++) {
        int cnt = *op & 15, shift = (*op >> 4) & 15, tgt = (*op >> 8) & 15;
        uint32_t v = (*op & 0x8000u) ? br.get_rev(cnt) : br.get(cnt);
        field[tgt >> 2][tgt & 3] |= (int)v << shift;
    }
    int partition = m.partitioned ? (int)br.get(5) : 0;
    int nep = m.partitioned ? 4 : 2;

    if (sgn) {
        r[0] = b6_sext(r[0], m.epb);
        g[0] = b6_sext(g[0], m.epb);
        b[0] = b6_sext(b[0], m.epb);
    }
    if (m.transformed || sgn) {
        for (int e = 1; e < nep; e++) {
            r[e] = b6_sext(r[e], m.db[0]);
            g[e] = b6_sext(g[e], m.db[1]);
            b[e] = b6_sext(b[e], m.db[2]);
        }
    }
    if (m.transformed) {                /* delta decompression */
        int mask = (1 << m.epb) - 1;
        for (int e = 1; e < nep; e++) {
            r[e] = (r[e] + r[0]) & mask;
            g[e] = (g[e] + g[0]) & mask;
            b[e] = (b[e] + b[0]) & mask;
            if (sgn) {
                r[e] = b6_sext(r[e], m.epb);
                g[e] = b6_sext(g[e], m.epb);
                b[e] = b6_sext(b[e], m.epb);
            }
        }
    }
    for (int e = 0; e < nep; e++) {
        r[e] = b6_unquantize(r[e], m.epb, sgn);
        g[e] = b6_unquantize(g[e], m.epb, sgn);
        b[e] = b6_unquantize(b[e], m.epb, sgn);
    }

    const int *w = m.partitioned ? kW3 : kW4;
    int ib = m.partitioned ? 3 : 4;
    for (int p = 0; p < 16; p++) {
        int subset = 0, anchor = (p == 0);
        if (m.partitioned) {
            subset = (kP2[partition] >> p) & 1;
            anchor = (p == 0) || (p == kA2[partition]);
        }
        int idx = (int)br.get(anchor ? ib - 1 : ib);
        int e0 = subset * 2, e1 = e0 + 1;
        outh[p * 3 + 0] = b6_finish_unquantize(interp64(r[e0], r[e1], w[idx]), sgn);
        outh[p * 3 + 1] = b6_finish_unquantize(interp64(g[e0], g[e1], w[idx]), sgn);
        outh[p * 3 + 2] = b6_finish_unquantize(interp64(b[e0], b[e1], w[idx]), sgn);
    }
}

/* ------------------------------------------------- encoder utilities --- */

/* Mean + principal axis (power iteration) of n dim-D points. */
static void pca_mean_axis(const float pts[][4], int n, int dim,
                          float mean[4], float axis[4]) {
    for (int k = 0; k < 4; k++) mean[k] = 0.0f;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < dim; k++) mean[k] += pts[i][k];
    for (int k = 0; k < dim; k++) mean[k] /= (float)n;

    float c[4][4] = { { 0 } };
    for (int i = 0; i < n; i++) {
        float d[4];
        for (int k = 0; k < dim; k++) d[k] = pts[i][k] - mean[k];
        for (int j = 0; j < dim; j++)
            for (int k = 0; k < dim; k++) c[j][k] += d[j] * d[k];
    }
    /* Seed with the covariance row of largest diagonal: for (near-)rank-1
     * data that row is already the principal axis, and it can never be
     * annihilated the way a fixed seed vector can. */
    int dmax = 0;
    for (int k = 1; k < dim; k++)
        if (c[k][k] > c[dmax][dmax]) dmax = k;
    for (int k = 0; k < 4; k++) axis[k] = 0.0f;
    if (c[dmax][dmax] <= 1e-10f)
        return;                         /* flat block: zero axis, e0=e1=mean */
    for (int k = 0; k < dim; k++) axis[k] = c[dmax][k];
    for (int it = 0; it < 6; it++) {
        float w[4] = { 0, 0, 0, 0 };
        for (int j = 0; j < dim; j++)
            for (int k = 0; k < dim; k++) w[j] += c[j][k] * axis[k];
        float nrm = 0.0f;
        for (int j = 0; j < dim; j++) nrm += w[j] * w[j];
        nrm = sqrtf(nrm);
        if (nrm < 1e-8f) break;
        for (int j = 0; j < dim; j++) axis[j] = w[j] / nrm;
    }
    float nrm = 0.0f;
    for (int j = 0; j < dim; j++) nrm += axis[j] * axis[j];
    nrm = sqrtf(nrm);
    if (nrm > 1e-8f)
        for (int j = 0; j < dim; j++) axis[j] /= nrm;
}

/* Initial endpoints: min/max projections onto the principal axis. */
static void fit_line_endpoints(const float pts[][4], int n, int dim,
                               float e0[4], float e1[4]) {
    float mean[4], ax[4];
    pca_mean_axis(pts, n, dim, mean, ax);
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < n; i++) {
        float t = 0.0f;
        for (int k = 0; k < dim; k++) t += (pts[i][k] - mean[k]) * ax[k];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    for (int k = 0; k < dim; k++) {
        e0[k] = mean[k] + tmin * ax[k];
        e1[k] = mean[k] + tmax * ax[k];
    }
}

/* Least-squares endpoint refit: value ~= A*(1-w) + B*w with per-pixel
 * weight w = wtab[idx[i]]. Returns 0 if the system is degenerate. */
static int lsq_endpoints(const float pts[][4], const uint8_t *idx,
                         const float *wtab, int n, int dim,
                         float e0[4], float e1[4]) {
    float a00 = 0, a01 = 0, a11 = 0, bx[4] = { 0, 0, 0, 0 }, by[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        float w = wtab[idx[i]], iw = 1.0f - w;
        a00 += iw * iw; a01 += iw * w; a11 += w * w;
        for (int k = 0; k < dim; k++) {
            bx[k] += iw * pts[i][k];
            by[k] += w * pts[i][k];
        }
    }
    float det = a00 * a11 - a01 * a01;
    if (fabsf(det) < 1e-4f * (a00 + a11 + 1e-6f)) return 0;
    for (int k = 0; k < dim; k++) {
        float n0 = (a11 * bx[k] - a01 * by[k]) / det;
        float n1 = (a00 * by[k] - a01 * bx[k]) / det;
        if (n0 != n0 || n1 != n1) return 0;     /* NaN guard */
        e0[k] = n0;
        e1[k] = n1;
    }
    return 1;
}

/* ---------------------------------------------------- BC1/2/3 encode --- */

static uint16_t pack565(const float e[4]) {
    int r = (int)(e[0] * (31.0f / 255.0f) + 0.5f);
    int gq = (int)(e[1] * (63.0f / 255.0f) + 0.5f);
    int b = (int)(e[2] * (31.0f / 255.0f) + 0.5f);
    if (r < 0) r = 0; if (r > 31) r = 31;
    if (gq < 0) gq = 0; if (gq > 63) gq = 63;
    if (b < 0) b = 0; if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (gq << 5) | b);
}

static void encode_bc1_block(const uint8_t px[64], uint8_t out[8], int allow_punch,
                             int punch_thresh) {
    int punch = 0;
    if (allow_punch)
        for (int p = 0; p < 16; p++)
            if (px[p * 4 + 3] < punch_thresh) { punch = 1; break; }

    /* Fit points: opaque pixels only in punch-through mode. */
    float pts[16][4];
    uint8_t pmap[16];
    int np = 0;
    for (int p = 0; p < 16; p++) {
        if (punch && px[p * 4 + 3] < punch_thresh) continue;
        pts[np][0] = px[p * 4 + 0];
        pts[np][1] = px[p * 4 + 1];
        pts[np][2] = px[p * 4 + 2];
        pts[np][3] = 0.0f;
        pmap[np++] = (uint8_t)p;
    }
    if (np == 0) {                      /* fully transparent: all index 3 */
        out[0] = out[1] = out[2] = out[3] = 0;
        out[4] = out[5] = out[6] = out[7] = 0xFF;
        return;
    }

    float e0[4], e1[4];
    fit_line_endpoints(pts, np, 3, e0, e1);

    /* Weight (fraction toward colour 1) per palette index. */
    static const float kWOpq[4]   = { 0.0f, 1.0f, 1.0f / 3.0f, 2.0f / 3.0f };
    static const float kWPunch[4] = { 0.0f, 1.0f, 0.5f, 0.0f };

    uint16_t best_c0 = 0, best_c1 = 0;
    uint8_t best_idx[16];
    memset(best_idx, 0, 16);
    float best_err = 1e30f;

    for (int iter = 0; iter < 3; iter++) {
        uint16_t c0 = pack565(e0), c1 = pack565(e1);
        if (punch) { if (c0 > c1) { uint16_t t = c0; c0 = c1; c1 = t; } }
        else       { if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; } }

        uint8_t pal[4][4];
        bc1_palette(c0, c1, 0, pal);
        int ncand = punch ? 3 : ((c0 == c1) ? 1 : 4);

        uint8_t idx[16];
        uint8_t fit_idx[16];            /* palette index per fit point */
        float err = 0.0f;
        for (int p = 0; p < 16; p++) idx[p] = punch ? 3 : 0;
        for (int i = 0; i < np; i++) {
            int bi = 0;
            float bd = 1e30f;
            for (int k = 0; k < ncand; k++) {
                float dr = pts[i][0] - pal[k][0];
                float dg = pts[i][1] - pal[k][1];
                float db = pts[i][2] - pal[k][2];
                float d = dr * dr + dg * dg + db * db;
                if (d < bd) { bd = d; bi = k; }
            }
            idx[pmap[i]] = (uint8_t)bi;
            fit_idx[i] = (uint8_t)bi;
            err += bd;
        }
        if (err < best_err) {
            best_err = err;
            best_c0 = c0;
            best_c1 = c1;
            memcpy(best_idx, idx, 16);
        }
        if (iter == 2) break;
        if (!lsq_endpoints(pts, fit_idx, punch ? kWPunch : kWOpq, np, 3, e0, e1))
            break;
    }

    out[0] = (uint8_t)(best_c0 & 0xFF); out[1] = (uint8_t)(best_c0 >> 8);
    out[2] = (uint8_t)(best_c1 & 0xFF); out[3] = (uint8_t)(best_c1 >> 8);
    for (int row = 0; row < 4; row++)
        out[4 + row] = (uint8_t)(best_idx[row * 4 + 0] |
                                 (best_idx[row * 4 + 1] << 2) |
                                 (best_idx[row * 4 + 2] << 4) |
                                 (best_idx[row * 4 + 3] << 6));
}

/* Encode a single channel of 16 values as an 8-byte BC4-style block. */
static void encode_bc4_block(const int v[16], int sgn, uint8_t out[8]) {
    int lo = v[0], hi = v[0];
    for (int p = 1; p < 16; p++) {
        if (v[p] < lo) lo = v[p];
        if (v[p] > hi) hi = v[p];
    }
    uint64_t bits = 0;
    int a0 = hi, a1 = lo;
    if (lo != hi) {                     /* 8-value mode requires a0 > a1 */
        int pal[8];
        bc4_palette(a0, a1, sgn, pal);
        for (int p = 0; p < 16; p++) {
            int bi = 0, bd = 0x7FFFFFFF;
            for (int k = 0; k < 8; k++) {
                int d = v[p] - pal[k];
                d *= d;
                if (d < bd) { bd = d; bi = k; }
            }
            bits |= (uint64_t)bi << (3 * p);
        }
    } /* else: a0 == a1, all indices 0 -> exact */
    out[0] = (uint8_t)(a0 & 0xFF);
    out[1] = (uint8_t)(a1 & 0xFF);
    for (int i = 0; i < 6; i++)
        out[2 + i] = (uint8_t)(bits >> (8 * i));
}

static void encode_bc2_block(const uint8_t px[64], uint8_t out[16]) {
    for (int row = 0; row < 4; row++) {
        uint16_t a = 0;
        for (int col = 0; col < 4; col++)
            a |= (uint16_t)(((px[(row * 4 + col) * 4 + 3] + 8) / 17) << (col * 4));
        out[row * 2 + 0] = (uint8_t)(a & 0xFF);
        out[row * 2 + 1] = (uint8_t)(a >> 8);
    }
    encode_bc1_block(px, out + 8, 0, 128);
}

static void encode_bc3_block(const uint8_t px[64], uint8_t out[16]) {
    int a[16];
    for (int p = 0; p < 16; p++) a[p] = px[p * 4 + 3];
    encode_bc4_block(a, 0, out);
    encode_bc1_block(px, out + 8, 0, 128);
}

/* ------------------------------------------------- BC7 mode 6 encode --- */
/* Single subset, RGBA 7.7.7.7 endpoints + per-endpoint P-bit, 4-bit
 * indices, endpoint refinement by alternating index fit / least squares. */

static void encode_bc7_block(const uint8_t px[64], uint8_t out[16]) {
    float pts[16][4];
    for (int p = 0; p < 16; p++)
        for (int c = 0; c < 4; c++)
            pts[p][c] = px[p * 4 + c];

    float e0[4], e1[4];
    fit_line_endpoints(pts, 16, 4, e0, e1);

    static const float kWF[16] = {
        0.0f, 4/64.0f, 9/64.0f, 13/64.0f, 17/64.0f, 21/64.0f, 26/64.0f, 30/64.0f,
        34/64.0f, 38/64.0f, 43/64.0f, 47/64.0f, 51/64.0f, 55/64.0f, 60/64.0f, 1.0f
    };

    int bq0[4] = { 0, 0, 0, 0 }, bq1[4] = { 0, 0, 0, 0 };
    int bp0 = 0, bp1 = 0;
    uint8_t bidx[16];
    memset(bidx, 0, 16);
    float best_err = 1e30f;

    for (int iter = 0; iter < 3; iter++) {
        uint8_t round_idx[16];
        int have_round = 0;
        for (int p0 = 0; p0 <= 1; p0++)
        for (int p1 = 0; p1 <= 1; p1++) {
            int q0[4], q1[4], r0[4], r1[4];
            for (int c = 0; c < 4; c++) {
                q0[c] = (int)((e0[c] - p0) * 0.5f + 0.5f);
                q1[c] = (int)((e1[c] - p1) * 0.5f + 0.5f);
                if (q0[c] < 0) q0[c] = 0; if (q0[c] > 127) q0[c] = 127;
                if (q1[c] < 0) q1[c] = 0; if (q1[c] > 127) q1[c] = 127;
                r0[c] = (q0[c] << 1) | p0;      /* exact 8-bit reconstruction */
                r1[c] = (q1[c] << 1) | p1;
            }
            int pal[16][4];
            for (int i = 0; i < 16; i++)
                for (int c = 0; c < 4; c++)
                    pal[i][c] = interp64(r0[c], r1[c], kW4[i]);
            uint8_t idx[16];
            float err = 0.0f;
            for (int p = 0; p < 16; p++) {
                int bi = 0;
                float bd = 1e30f;
                for (int i = 0; i < 16; i++) {
                    float d = 0.0f;
                    for (int c = 0; c < 4; c++) {
                        float dc = pts[p][c] - pal[i][c];
                        d += dc * dc;
                    }
                    if (d < bd) { bd = d; bi = i; }
                }
                idx[p] = (uint8_t)bi;
                err += bd;
            }
            if (err < best_err) {
                best_err = err;
                memcpy(bq0, q0, sizeof(q0));
                memcpy(bq1, q1, sizeof(q1));
                bp0 = p0; bp1 = p1;
                memcpy(bidx, idx, 16);
            }
            if (!have_round || err <= best_err) {
                memcpy(round_idx, idx, 16);
                have_round = 1;
            }
        }
        if (iter == 2) break;
        if (!lsq_endpoints(pts, round_idx, kWF, 16, 4, e0, e1))
            break;
        for (int c = 0; c < 4; c++) {   /* keep refits in the valid domain */
            if (e0[c] < 0.0f) e0[c] = 0.0f;
            if (e0[c] > 255.0f) e0[c] = 255.0f;
            if (e1[c] < 0.0f) e1[c] = 0.0f;
            if (e1[c] > 255.0f) e1[c] = 255.0f;
        }
    }

    /* Anchor: index 0 is stored with 3 bits (MSB must be 0). */
    if (bidx[0] > 7) {
        for (int c = 0; c < 4; c++) { int t = bq0[c]; bq0[c] = bq1[c]; bq1[c] = t; }
        int t = bp0; bp0 = bp1; bp1 = t;
        for (int p = 0; p < 16; p++) bidx[p] = (uint8_t)(15 - bidx[p]);
    }

    memset(out, 0, 16);
    BitW bw = { out, 0 };
    bw.put(1u << 6, 7);                 /* mode 6: six zeros then a one */
    for (int c = 0; c < 4; c++) {
        bw.put((uint32_t)bq0[c], 7);
        bw.put((uint32_t)bq1[c], 7);
    }
    bw.put((uint32_t)bp0, 1);
    bw.put((uint32_t)bp1, 1);
    bw.put(bidx[0], 3);
    for (int p = 1; p < 16; p++) bw.put(bidx[p], 4);
}

/* ------------------------------------------------ BC6H mode 11 encode -- */
/* Single region, direct 10-bit endpoints, 4-bit indices. Works in the
 * "half-as-int" domain (monotonic in value; signed uses sign-magnitude
 * converted to a plain int). */

static inline int b6_half_to_int(uint16_t h, int sgn) {
    if (!sgn) return h;
    return (h & 0x8000) ? -(int)(h & 0x7FFF) : (int)h;
}

/* Reconstructed value (half-as-int domain) of a 10-bit endpoint code. */
static int b6_dequant10(int x, int sgn) {
    uint16_t h = b6_finish_unquantize(b6_unquantize(x, 10, sgn), sgn);
    return b6_half_to_int(h, sgn);
}

/* Best 10-bit endpoint code for a target half-as-int value. */
static int b6_quant10(float target, int sgn) {
    int t = (int)floorf(target + 0.5f);
    int lo_lim = sgn ? -511 : 0, hi_lim = sgn ? 511 : 1023;
    int x0 = sgn ? t / 62 : t / 31;     /* dequant slope is ~62 / ~31 */
    int lo = x0 - 2, hi = x0 + 2;
    if (lo < lo_lim) lo = lo_lim;
    if (hi > hi_lim) hi = hi_lim;
    int best = lo, bd = 0x7FFFFFFF;
    for (int x = lo; x <= hi; x++) {
        int d = b6_dequant10(x, sgn) - t;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = x; }
    }
    return best;
}

/* Encode a 4x4 block of float RGB(A) into a BC6H mode 11 block. */
static void encode_bc6h_block(const float *pxf, int sgn, uint8_t out[16]) {
    static const float kWF[16] = {
        0.0f, 4/64.0f, 9/64.0f, 13/64.0f, 17/64.0f, 21/64.0f, 26/64.0f, 30/64.0f,
        34/64.0f, 38/64.0f, 43/64.0f, 47/64.0f, 51/64.0f, 55/64.0f, 60/64.0f, 1.0f
    };
    float pts[16][4];                   /* half-as-int domain, [3] unused */
    for (int p = 0; p < 16; p++) {
        for (int c = 0; c < 3; c++) {
            float v = pxf[p * 4 + c];
            if (v != v) v = 0.0f;       /* NaN */
            if (sgn) {
                if (v < -65504.0f) v = -65504.0f;
                if (v > 65504.0f) v = 65504.0f;
            } else {
                if (!(v > 0.0f)) v = 0.0f;
                if (v > 65504.0f) v = 65504.0f;
            }
            pts[p][c] = (float)b6_half_to_int(float_to_half(v), sgn);
        }
        pts[p][3] = 0.0f;
    }

    /* Initial endpoints: PCA line fit (a per-channel min/max box diagonal
     * mis-orients the palette line for anticorrelated channels). */
    float e0[4], e1[4];
    fit_line_endpoints(pts, 16, 3, e0, e1);
    float tlo = sgn ? -31743.0f : 0.0f, thi = 31743.0f;
    for (int c = 0; c < 3; c++) {
        if (e0[c] < tlo) e0[c] = tlo;
        if (e0[c] > thi) e0[c] = thi;
        if (e1[c] < tlo) e1[c] = tlo;
        if (e1[c] > thi) e1[c] = thi;
    }

    int bq0[3] = { 0, 0, 0 }, bq1[3] = { 0, 0, 0 };
    uint8_t bidx[16];
    memset(bidx, 0, 16);
    float best_err = 1e30f;

    for (int iter = 0; iter < 3; iter++) {
        int q0[3], q1[3];
        float pal[16][3];
        for (int c = 0; c < 3; c++) {
            q0[c] = b6_quant10(e0[c], sgn);
            q1[c] = b6_quant10(e1[c], sgn);
        }
        int u0[3], u1[3];
        for (int c = 0; c < 3; c++) {
            u0[c] = b6_unquantize(q0[c], 10, sgn);
            u1[c] = b6_unquantize(q1[c], 10, sgn);
        }
        for (int i = 0; i < 16; i++)
            for (int c = 0; c < 3; c++)
                pal[i][c] = (float)b6_half_to_int(
                    b6_finish_unquantize(interp64(u0[c], u1[c], kW4[i]), sgn), sgn);

        uint8_t idx[16];
        float err = 0.0f;
        for (int p = 0; p < 16; p++) {
            int bi = 0;
            float bd = 1e30f;
            for (int i = 0; i < 16; i++) {
                float d = 0.0f;
                for (int c = 0; c < 3; c++) {
                    float dc = pts[p][c] - pal[i][c];
                    d += dc * dc;
                }
                if (d < bd) { bd = d; bi = i; }
            }
            idx[p] = (uint8_t)bi;
            err += bd;
        }
        if (err < best_err) {
            best_err = err;
            memcpy(bq0, q0, sizeof(q0));
            memcpy(bq1, q1, sizeof(q1));
            memcpy(bidx, idx, 16);
        }
        if (iter == 2) break;
        if (!lsq_endpoints(pts, idx, kWF, 16, 3, e0, e1))
            break;
        for (int c = 0; c < 3; c++) {   /* keep refits in the valid domain */
            if (e0[c] < tlo) e0[c] = tlo;
            if (e0[c] > thi) e0[c] = thi;
            if (e1[c] < tlo) e1[c] = tlo;
            if (e1[c] > thi) e1[c] = thi;
        }
    }

    if (bidx[0] > 7) {                  /* anchor: pixel 0 stored in 3 bits */
        for (int c = 0; c < 3; c++) { int t = bq0[c]; bq0[c] = bq1[c]; bq1[c] = t; }
        for (int p = 0; p < 16; p++) bidx[p] = (uint8_t)(15 - bidx[p]);
    }

    memset(out, 0, 16);
    BitW bw = { out, 0 };
    bw.put(0x03, 5);                    /* mode 11 = 00011 (LSB first) */
    for (int c = 0; c < 3; c++) bw.put((uint32_t)bq0[c] & 0x3FF, 10);
    for (int c = 0; c < 3; c++) bw.put((uint32_t)bq1[c] & 0x3FF, 10);
    bw.put(bidx[0], 3);
    for (int p = 1; p < 16; p++) bw.put(bidx[p], 4);
}

/* -------------------------------------------------------- half -> u8 --- */

static void bc6h_halves_to_rgba8(const uint16_t h[48], uint8_t out[64]) {
    for (int p = 0; p < 16; p++) {
        for (int c = 0; c < 3; c++) {
            float v = clamp01(half_to_float(h[p * 3 + c]));
            out[p * 4 + c] = (uint8_t)(v * 255.0f + 0.5f);
        }
        out[p * 4 + 3] = 255;
    }
}

} /* anonymous namespace */

/* ==================================================== module entries === */

int bcn_decode(texc_format fmt, const uint8_t *src, size_t src_size,
               uint32_t width, uint32_t height, uint8_t *dst) {
    (void)src_size;
    uint32_t nbx = (width + 3) / 4, nby = (height + 3) / 4;
    size_t bs = (fmt == TEXC_FORMAT_BC1 || fmt == TEXC_FORMAT_BC4 ||
                 fmt == TEXC_FORMAT_BC4_SNORM) ? 8 : 16;

    for (uint32_t by = 0; by < nby; by++)
    for (uint32_t bx = 0; bx < nbx; bx++) {
        const uint8_t *s = src + ((size_t)by * nbx + bx) * bs;
        uint8_t blk[64];
        switch (fmt) {
            case TEXC_FORMAT_BC1:
                decode_bc1_block(s, blk, 0);
                break;
            case TEXC_FORMAT_BC2: {
                decode_bc1_block(s + 8, blk, 1);
                for (int row = 0; row < 4; row++) {
                    uint16_t a = (uint16_t)(s[row * 2] | (s[row * 2 + 1] << 8));
                    for (int col = 0; col < 4; col++)
                        blk[(row * 4 + col) * 4 + 3] =
                            (uint8_t)(((a >> (col * 4)) & 0x0F) * 17);
                }
            } break;
            case TEXC_FORMAT_BC3: {
                decode_bc1_block(s + 8, blk, 1);
                int a[16];
                decode_bc4_values(s, 0, a);
                for (int p = 0; p < 16; p++)
                    blk[p * 4 + 3] = (uint8_t)a[p];
            } break;
            case TEXC_FORMAT_BC4:
            case TEXC_FORMAT_BC4_SNORM: {
                int sgn = (fmt == TEXC_FORMAT_BC4_SNORM);
                int v[16];
                decode_bc4_values(s, sgn, v);
                for (int p = 0; p < 16; p++) {
                    blk[p * 4 + 0] = sgn ? snorm_to_u8(v[p]) : (uint8_t)v[p];
                    blk[p * 4 + 1] = 0;
                    blk[p * 4 + 2] = 0;
                    blk[p * 4 + 3] = 255;
                }
            } break;
            case TEXC_FORMAT_BC5:
            case TEXC_FORMAT_BC5_SNORM: {
                int sgn = (fmt == TEXC_FORMAT_BC5_SNORM);
                int r[16], g[16];
                decode_bc4_values(s, sgn, r);
                decode_bc4_values(s + 8, sgn, g);
                for (int p = 0; p < 16; p++) {
                    blk[p * 4 + 0] = sgn ? snorm_to_u8(r[p]) : (uint8_t)r[p];
                    blk[p * 4 + 1] = sgn ? snorm_to_u8(g[p]) : (uint8_t)g[p];
                    blk[p * 4 + 2] = 0;
                    blk[p * 4 + 3] = 255;
                }
            } break;
            case TEXC_FORMAT_BC6H_UF16:
            case TEXC_FORMAT_BC6H_SF16: {
                uint16_t h[48];
                decode_bc6h_block(s, fmt == TEXC_FORMAT_BC6H_SF16, h);
                bc6h_halves_to_rgba8(h, blk);
            } break;
            case TEXC_FORMAT_BC7:
                decode_bc7_block(s, blk);
                break;
            default:
                return TEXC_ERR_UNSUPPORTED;
        }
        write_block_rgba8(dst, width, height, bx, by, 4, 4, blk);
    }
    return TEXC_OK;
}

int bcn_decode_f32(texc_format fmt, const uint8_t *src, size_t src_size,
                   uint32_t width, uint32_t height, float *dst) {
    (void)src_size;
    if (fmt != TEXC_FORMAT_BC6H_UF16 && fmt != TEXC_FORMAT_BC6H_SF16)
        return TEXC_ERR_UNSUPPORTED;
    int sgn = (fmt == TEXC_FORMAT_BC6H_SF16);
    uint32_t nbx = (width + 3) / 4, nby = (height + 3) / 4;

    for (uint32_t by = 0; by < nby; by++)
    for (uint32_t bx = 0; bx < nbx; bx++) {
        const uint8_t *s = src + ((size_t)by * nbx + bx) * 16;
        uint16_t h[48];
        float blk[64];
        decode_bc6h_block(s, sgn, h);
        for (int p = 0; p < 16; p++) {
            blk[p * 4 + 0] = half_to_float(h[p * 3 + 0]);
            blk[p * 4 + 1] = half_to_float(h[p * 3 + 1]);
            blk[p * 4 + 2] = half_to_float(h[p * 3 + 2]);
            blk[p * 4 + 3] = 1.0f;
        }
        write_block_rgba32f(dst, width, height, bx, by, 4, 4, blk);
    }
    return TEXC_OK;
}

int bcn_encode(texc_format fmt, const uint8_t *src,
               uint32_t width, uint32_t height, uint8_t *dst,
               const texc_encode_options *opts) {
    uint32_t nbx = (width + 3) / 4, nby = (height + 3) / 4;
    size_t bs = (fmt == TEXC_FORMAT_BC1 || fmt == TEXC_FORMAT_BC4 ||
                 fmt == TEXC_FORMAT_BC4_SNORM) ? 8 : 16;
    int punch_thresh = (int)(opts ? opts->alpha_threshold : 128u);
    if (punch_thresh > 256) punch_thresh = 256;

    for (uint32_t by = 0; by < nby; by++)
    for (uint32_t bx = 0; bx < nbx; bx++) {
        uint8_t *d = dst + ((size_t)by * nbx + bx) * bs;
        uint8_t blk[64];
        read_block_rgba8(src, width, height, bx, by, 4, 4, blk);
        switch (fmt) {
            case TEXC_FORMAT_BC1:
                encode_bc1_block(blk, d, 1, punch_thresh);
                break;
            case TEXC_FORMAT_BC2:
                encode_bc2_block(blk, d);
                break;
            case TEXC_FORMAT_BC3:
                encode_bc3_block(blk, d);
                break;
            case TEXC_FORMAT_BC4:
            case TEXC_FORMAT_BC4_SNORM: {
                int sgn = (fmt == TEXC_FORMAT_BC4_SNORM);
                int v[16];
                for (int p = 0; p < 16; p++)
                    v[p] = sgn ? u8_to_snorm(blk[p * 4]) : blk[p * 4];
                encode_bc4_block(v, sgn, d);
            } break;
            case TEXC_FORMAT_BC5:
            case TEXC_FORMAT_BC5_SNORM: {
                int sgn = (fmt == TEXC_FORMAT_BC5_SNORM);
                int r[16], g[16];
                for (int p = 0; p < 16; p++) {
                    r[p] = sgn ? u8_to_snorm(blk[p * 4 + 0]) : blk[p * 4 + 0];
                    g[p] = sgn ? u8_to_snorm(blk[p * 4 + 1]) : blk[p * 4 + 1];
                }
                encode_bc4_block(r, sgn, d);
                encode_bc4_block(g, sgn, d + 8);
            } break;
            case TEXC_FORMAT_BC6H_UF16:
            case TEXC_FORMAT_BC6H_SF16: {
                float f[64];
                for (int i = 0; i < 64; i++)
                    f[i] = (float)blk[i] / 255.0f;
                encode_bc6h_block(f, fmt == TEXC_FORMAT_BC6H_SF16, d);
            } break;
            case TEXC_FORMAT_BC7:
                encode_bc7_block(blk, d);
                break;
            default:
                return TEXC_ERR_UNSUPPORTED;
        }
    }
    return TEXC_OK;
}

int bcn_encode_f32(texc_format fmt, const float *src,
                   uint32_t width, uint32_t height, uint8_t *dst) {
    if (fmt != TEXC_FORMAT_BC6H_UF16 && fmt != TEXC_FORMAT_BC6H_SF16)
        return TEXC_ERR_UNSUPPORTED;
    int sgn = (fmt == TEXC_FORMAT_BC6H_SF16);
    uint32_t nbx = (width + 3) / 4, nby = (height + 3) / 4;

    for (uint32_t by = 0; by < nby; by++)
    for (uint32_t bx = 0; bx < nbx; bx++) {
        float blk[64];
        read_block_rgba32f(src, width, height, bx, by, 4, 4, blk);
        encode_bc6h_block(blk, sgn, dst + ((size_t)by * nbx + bx) * 16);
    }
    return TEXC_OK;
}

} /* namespace texc */

/* ===================================================== self test ====== */
#ifdef TEXC_SELFTEST

#include <stdio.h>

using namespace texc;

static int g_fail = 0;
static uint32_t g_seed = 1;
static uint32_t xrnd(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

#define MAXPX (64 * 64)
static uint8_t s_img[MAXPX * 4], s_dec[MAXPX * 4];
static uint8_t s_enc[MAXPX];
static float s_fimg[MAXPX * 4], s_fdec[MAXPX * 4];

/* Diagonal colour ramp: all channels are linear in one parameter so a
 * block's pixels are colinear in colour space (the case the per-format
 * PSNR bars assume). */
static void make_gradient(uint8_t *img, int w, int h, int alpha_gradient) {
    int dw = w > 1 ? w - 1 : 1, dh = h > 1 ? h - 1 : 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = img + (y * w + x) * 4;
            int t = (x * 255 / dw + y * 255 / dh) / 2;
            p[0] = (uint8_t)t;
            p[1] = (uint8_t)(255 - t);
            p[2] = (uint8_t)(64 + t / 2);
            p[3] = alpha_gradient ? (uint8_t)(255 - t / 2) : 255;
        }
}

static void make_random(uint8_t *img, int w, int h, uint32_t seed, int opaque) {
    g_seed = seed;
    for (int i = 0; i < w * h; i++) {
        img[i * 4 + 0] = (uint8_t)xrnd();
        img[i * 4 + 1] = (uint8_t)xrnd();
        img[i * 4 + 2] = (uint8_t)xrnd();
        img[i * 4 + 3] = opaque ? 255 : (uint8_t)xrnd();
    }
}

static double psnr_mask(const uint8_t *a, const uint8_t *b, int npx, int mask) {
    double mse = 0.0;
    int cnt = 0;
    for (int i = 0; i < npx; i++)
        for (int c = 0; c < 4; c++)
            if (mask & (1 << c)) {
                double d = (double)a[i * 4 + c] - (double)b[i * 4 + c];
                mse += d * d;
                cnt++;
            }
    if (!cnt) return 99.0;
    mse /= cnt;
    if (mse <= 1e-12) return 99.0;
    double v = 10.0 * log10(255.0 * 255.0 / mse);
    return v > 99.0 ? 99.0 : v;
}

static void check_db(const char *name, double db, double thr) {
    int ok = db >= thr;
    printf("  %-28s %6.2f dB (>= %.0f)  %s\n", name, db, thr, ok ? "ok" : "FAIL");
    if (!ok) g_fail = 1;
}

static void test_ldr(const char *name, texc_format fmt, const uint8_t *img,
                     int w, int h, int mask, double thr) {
    if (bcn_encode(fmt, img, (uint32_t)w, (uint32_t)h, s_enc, nullptr) != TEXC_OK ||
        bcn_decode(fmt, s_enc, sizeof(s_enc), (uint32_t)w, (uint32_t)h, s_dec) != TEXC_OK) {
        printf("  %-28s encode/decode error FAIL\n", name);
        g_fail = 1;
        return;
    }
    check_db(name, psnr_mask(img, s_dec, w * h, mask), thr);
}

static void test_bc1_punch(int w, int h) {
    /* Block-coherent binary-alpha checkerboard over a gradient. */
    make_gradient(s_img, w, h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            s_img[(y * w + x) * 4 + 3] = (uint8_t)((((x / 2) + (y / 2)) & 1) ? 255 : 0);

    bcn_encode(TEXC_FORMAT_BC1, s_img, (uint32_t)w, (uint32_t)h, s_enc, nullptr);
    bcn_decode(TEXC_FORMAT_BC1, s_enc, sizeof(s_enc), (uint32_t)w, (uint32_t)h, s_dec);

    int alpha_ok = 1;
    double mse = 0.0;
    int cnt = 0;
    for (int i = 0; i < w * h; i++) {
        uint8_t want_a = s_img[i * 4 + 3] >= 128 ? 255 : 0;
        if (s_dec[i * 4 + 3] != want_a) alpha_ok = 0;
        if (want_a == 255)
            for (int c = 0; c < 3; c++) {
                double d = (double)s_img[i * 4 + c] - (double)s_dec[i * 4 + c];
                mse += d * d;
                cnt++;
            }
    }
    mse = cnt ? mse / cnt : 0.0;
    double db = mse <= 1e-12 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
    printf("  %-28s alpha %s\n", "BC1 punch-through alpha", alpha_ok ? "exact ok" : "MISMATCH FAIL");
    if (!alpha_ok) g_fail = 1;
    check_db("BC1 punch-through RGB", db, 30.0);
}

static void test_bc6h(const char *name, texc_format fmt, int w, int h) {
    int sgn = (fmt == TEXC_FORMAT_BC6H_SF16);
    int dw = w > 1 ? w - 1 : 1, dh = h > 1 ? h - 1 : 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float *p = s_fimg + (y * w + x) * 4;
            float t = 2.0f * ((float)x / (float)dw + (float)y / (float)dh);
            p[0] = t;                   /* 0..4 */
            p[1] = 4.0f - t;
            p[2] = 0.1f + 0.5f * t;
            p[3] = 1.0f;
            if (sgn) { p[0] -= 2.0f; p[1] -= 2.0f; p[2] -= 2.0f; }
        }
    if (bcn_encode_f32(fmt, s_fimg, (uint32_t)w, (uint32_t)h, s_enc) != TEXC_OK ||
        bcn_decode_f32(fmt, s_enc, sizeof(s_enc), (uint32_t)w, (uint32_t)h, s_fdec) != TEXC_OK) {
        printf("  %-28s encode/decode error FAIL\n", name);
        g_fail = 1;
        return;
    }
    double maxrel = 0.0, sumrel = 0.0;
    int n = 0;
    for (int i = 0; i < w * h; i++)
        for (int c = 0; c < 3; c++) {
            double o = s_fimg[i * 4 + c], d = s_fdec[i * 4 + c];
            double denom = fabs(o);
            if (denom < 0.25) denom = 0.25;
            double rel = fabs(d - o) / denom;
            if (rel > maxrel) maxrel = rel;
            sumrel += rel;
            n++;
        }
    double mean = sumrel / n;
    /* BC6H interpolates in the half-float bit domain (quasi-logarithmic in
     * value), so blocks spanning near-zero to large values have palette
     * steps that are exponentially spaced in value; peak relative error of
     * a few tens of percent near zero is inherent to the format. Bars below
     * are "sane" for a mode 11 encoder (signed is coarser: 62 vs 31
     * bit-units per endpoint step, plus sign-crossing ramps). */
    double max_bar = sgn ? 0.75 : 0.50, mean_bar = sgn ? 0.08 : 0.05;
    int ok = maxrel < max_bar && mean < mean_bar;
    printf("  %-28s rel err max %.4f (<%.2f) mean %.5f (<%.2f)  %s\n",
           name, maxrel, max_bar, mean, mean_bar, ok ? "ok" : "FAIL");
    if (!ok) g_fail = 1;
}

static void test_kat(void) {
    /* BC1: c0 == c1 == pure red (0xF800), all indices 0 -> exact red. */
    static const uint8_t bc1_red[8] = { 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0 };
    bcn_decode(TEXC_FORMAT_BC1, bc1_red, 8, 4, 4, s_dec);
    int ok = 1;
    for (int p = 0; p < 16; p++)
        if (s_dec[p * 4 + 0] != 255 || s_dec[p * 4 + 1] != 0 ||
            s_dec[p * 4 + 2] != 0 || s_dec[p * 4 + 3] != 255) ok = 0;
    printf("  %-28s %s\n", "BC1 KAT (solid red)", ok ? "ok" : "FAIL");
    if (!ok) g_fail = 1;

    /* BC7 mode 6: all endpoint bits + both P-bits set -> endpoints are
     * (255,255,255,255); every index yields opaque white.
     * bits: [0..6] mode '0000001', [7..62] endpoints all ones,
     * [63..64] P-bits, [65..127] indices all zero. */
    static const uint8_t bc7_white[16] = {
        0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    bcn_decode(TEXC_FORMAT_BC7, bc7_white, 16, 4, 4, s_dec);
    ok = 1;
    for (int i = 0; i < 64; i++)
        if (s_dec[i] != 255) ok = 0;
    printf("  %-28s %s\n", "BC7 KAT (mode 6 white)", ok ? "ok" : "FAIL");
    if (!ok) g_fail = 1;

    /* BC7 encoder round-trip of a constant colour must be near-exact. */
    for (int p = 0; p < 16; p++) {
        s_img[p * 4 + 0] = 180; s_img[p * 4 + 1] = 90;
        s_img[p * 4 + 2] = 45;  s_img[p * 4 + 3] = 200;
    }
    bcn_encode(TEXC_FORMAT_BC7, s_img, 4, 4, s_enc, nullptr);
    bcn_decode(TEXC_FORMAT_BC7, s_enc, 16, 4, 4, s_dec);
    ok = 1;
    for (int i = 0; i < 64; i++) {
        int d = (int)s_img[i] - (int)s_dec[i];
        if (d < -1 || d > 1) ok = 0;
    }
    printf("  %-28s %s\n", "BC7 flat round-trip (+/-1)", ok ? "ok" : "FAIL");
    if (!ok) g_fail = 1;
}

static void run_size(int w, int h) {
    printf("--- %dx%d ---\n", w, h);
    const int RGB = 0x7, RGBA = 0xF, R = 0x1, RG = 0x3;

    make_gradient(s_img, w, h, 0);
    test_ldr("BC1 gradient", TEXC_FORMAT_BC1, s_img, w, h, RGB, 30.0);
    test_ldr("BC4 gradient", TEXC_FORMAT_BC4, s_img, w, h, R, 35.0);
    test_ldr("BC4snorm gradient", TEXC_FORMAT_BC4_SNORM, s_img, w, h, R, 35.0);
    test_ldr("BC5 gradient", TEXC_FORMAT_BC5, s_img, w, h, RG, 35.0);
    test_ldr("BC5snorm gradient", TEXC_FORMAT_BC5_SNORM, s_img, w, h, RG, 35.0);
    test_ldr("BC6H-u8 gradient (LDR)", TEXC_FORMAT_BC6H_UF16, s_img, w, h, RGB, 30.0);

    make_gradient(s_img, w, h, 1);
    test_ldr("BC2 gradient+alpha", TEXC_FORMAT_BC2, s_img, w, h, RGBA, 30.0);
    test_ldr("BC3 gradient+alpha", TEXC_FORMAT_BC3, s_img, w, h, RGBA, 30.0);
    test_ldr("BC7 gradient+alpha", TEXC_FORMAT_BC7, s_img, w, h, RGBA, 30.0);

    /* Noise images: sanity bound only (uncorrelated RGB(A) noise is the
     * pathological case for line-fit codecs; ~13-14 dB is normal BC1). */
    make_random(s_img, w, h, 0xC0FFEEu, 1);
    test_ldr("BC1 random", TEXC_FORMAT_BC1, s_img, w, h, RGB, 10.0);
    test_ldr("BC4 random", TEXC_FORMAT_BC4, s_img, w, h, R, 20.0);
    test_ldr("BC5 random", TEXC_FORMAT_BC5, s_img, w, h, RG, 20.0);
    make_random(s_img, w, h, 0xBADF00Du, 0);
    test_ldr("BC3 random+alpha", TEXC_FORMAT_BC3, s_img, w, h, RGBA, 10.0);
    test_ldr("BC7 random+alpha", TEXC_FORMAT_BC7, s_img, w, h, RGBA, 10.0);

    test_bc1_punch(w, h);
    test_bc6h("BC6H UF16 HDR gradient", TEXC_FORMAT_BC6H_UF16, w, h);
    test_bc6h("BC6H SF16 HDR gradient", TEXC_FORMAT_BC6H_SF16, w, h);
}

int main(void) {
    printf("--- known-answer tests ---\n");
    test_kat();
    run_size(16, 16);
    run_size(37, 23);
    printf(g_fail ? "SELFTEST FAILED\n" : "SELFTEST PASSED\n");
    return g_fail;
}

#endif /* TEXC_SELFTEST */
