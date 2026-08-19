/*
 * etc.cpp - ETC1 / ETC2 / EAC codec module (see codec_common.h).
 *
 * Decoders follow the OpenGL ES 3.0 specification, appendix C
 * ("ETC2/EAC compressed texture image formats") exactly:
 *   - individual (4+4) / differential (5+3) modes, flip bit, the 8 modifier
 *     tables, 2-bit pixel indices in msb/lsb split planes, pixels stored
 *     column-major (pixel index = x*4 + y),
 *   - ETC2 T / H / Planar modes signalled by red/green/blue diff overflow,
 *   - ETC2 punchthrough alpha (opaque bit reuses the diff bit; index 2 is
 *     transparent black and modifiers 0/2 are forced to zero when opaque=0),
 *   - EAC alpha / R11 / RG11 (+signed) blocks.
 *
 * Blocks are stored big-endian on disk: byte 0 holds bits 63..56.
 */

#include "codec_common.h"

#include <cmath>

namespace texc {
namespace {

/* -------------------------------------------------------------- tables --- */

/* Intensity modifier sets (spec table C.7/C.8). Indexed [table][pixel index]
 * where pixel index = msb*2 + lsb: 0 -> +a, 1 -> +b, 2 -> -a, 3 -> -b. */
const int kMod[8][4] = {
    {  2,   8,  -2,   -8 },
    {  5,  17,  -5,  -17 },
    {  9,  29,  -9,  -29 },
    { 13,  42, -13,  -42 },
    { 18,  60, -18,  -60 },
    { 24,  80, -24,  -80 },
    { 33, 106, -33, -106 },
    { 47, 183, -47, -183 },
};

/* T / H mode distance table (spec table C.9). */
const int kDist[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };

/* EAC modifier tables (spec table C.16), indexed [table][3-bit index]. */
const int kAlphaMod[16][8] = {
    { -3, -6,  -9, -15, 2, 5, 8, 14 },
    { -3, -7, -10, -13, 2, 6, 9, 12 },
    { -2, -5,  -8, -13, 1, 4, 7, 12 },
    { -2, -4,  -6, -13, 1, 3, 5, 12 },
    { -3, -6,  -8, -12, 2, 5, 7, 11 },
    { -3, -7,  -9, -11, 2, 6, 8, 10 },
    { -4, -7,  -8, -11, 3, 6, 7, 10 },
    { -3, -5,  -8, -11, 2, 4, 6, 10 },
    { -2, -6,  -8, -10, 1, 5, 7,  9 },
    { -2, -5,  -8, -10, 1, 4, 7,  9 },
    { -2, -4,  -8, -10, 1, 3, 7,  9 },
    { -2, -5,  -7, -10, 1, 4, 6,  9 },
    { -3, -4,  -7, -10, 2, 3, 6,  9 },
    { -1, -2,  -3, -10, 0, 1, 2,  9 },
    { -4, -6,  -8,  -9, 3, 5, 7,  8 },
    { -3, -5,  -7,  -9, 2, 4, 6,  8 },
};

/* ------------------------------------------------------------- helpers --- */

inline int sign3(int v) { return (v & 4) ? v - 8 : v; }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline uint32_t load32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
inline uint64_t load48be(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | p[i];
    return v;
}

inline uint8_t e6(int v) { return (uint8_t)((v << 2) | (v >> 4)); }   /* 6->8 */
inline uint8_t e7(int v) { return (uint8_t)((v << 1) | (v >> 6)); }   /* 7->8 */

inline uint64_t sqd3(const uint8_t *px, int r, int g, int b) {
    int dr = px[0] - r, dg = px[1] - g, db = px[2] - b;
    return (uint64_t)(dr * dr + dg * dg + db * db);
}

inline int quant4(double v) { return clampi((int)std::lround(v * 15.0 / 255.0), 0, 15); }
inline int quant5(double v) { return clampi((int)std::lround(v * 31.0 / 255.0), 0, 31); }

/* --------------------------------------------------- colour block decode --- */
/* All decode functions fill rgba[64] = 4x4 RGBA8, row-major within block.   */

/* Individual / differential paint: per-pixel table lookup around two bases. */
void decode_sub_indices(const uint8_t *p, const int base[2][3],
                        const int table[2], bool opaque, uint8_t rgba[64]) {
    uint32_t lo = load32be(p + 4);
    bool flip = (p[3] & 1) != 0;
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            int i = x * 4 + y;                       /* column-major index */
            int s = flip ? (y >> 1) : (x >> 1);
            int idx = (int)(((lo >> (16 + i)) & 1) * 2 + ((lo >> i) & 1));
            uint8_t *o = rgba + (y * 4 + x) * 4;
            if (!opaque && idx == 2) {               /* punchthrough hole   */
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            int m = kMod[table[s]][idx];
            if (!opaque && idx == 0) m = 0;          /* spec table C.12     */
            o[0] = clamp_u8(base[s][0] + m);
            o[1] = clamp_u8(base[s][1] + m);
            o[2] = clamp_u8(base[s][2] + m);
            o[3] = 255;
        }
    }
}

/* Shared T/H painter: 4 pre-clamped paint colours selected per pixel. */
void paint_pixels(const uint8_t *p, const int paint[4][3], bool opaque,
                  uint8_t rgba[64]) {
    uint32_t lo = load32be(p + 4);
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            int i = x * 4 + y;
            int idx = (int)(((lo >> (16 + i)) & 1) * 2 + ((lo >> i) & 1));
            uint8_t *o = rgba + (y * 4 + x) * 4;
            if (!opaque && idx == 2) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            o[0] = (uint8_t)paint[idx][0];
            o[1] = (uint8_t)paint[idx][1];
            o[2] = (uint8_t)paint[idx][2];
            o[3] = 255;
        }
    }
}

void decode_t_mode(const uint8_t *p, bool opaque, uint8_t rgba[64]) {
    int r1 = expand4(((p[0] & 0x18) >> 1) | (p[0] & 3));
    int g1 = expand4(p[1] >> 4);
    int b1 = expand4(p[1] & 15);
    int r2 = expand4(p[2] >> 4);
    int g2 = expand4(p[2] & 15);
    int b2 = expand4(p[3] >> 4);
    int d  = kDist[((p[3] >> 1) & 6) | (p[3] & 1)];
    const int paint[4][3] = {
        { r1, g1, b1 },
        { clamp_u8(r2 + d), clamp_u8(g2 + d), clamp_u8(b2 + d) },
        { r2, g2, b2 },
        { clamp_u8(r2 - d), clamp_u8(g2 - d), clamp_u8(b2 - d) },
    };
    paint_pixels(p, paint, opaque, rgba);
}

void decode_h_mode(const uint8_t *p, bool opaque, uint8_t rgba[64]) {
    int r1n = (p[0] >> 3) & 15;
    int g1n = ((p[0] & 7) << 1) | ((p[1] >> 4) & 1);
    int b1n = (p[1] & 8) | ((p[1] & 3) << 1) | (p[2] >> 7);
    int r2n = (p[2] >> 3) & 15;
    int g2n = ((p[2] & 7) << 1) | (p[3] >> 7);
    int b2n = (p[3] >> 3) & 15;
    int v1 = (r1n << 8) | (g1n << 4) | b1n;
    int v2 = (r2n << 8) | (g2n << 4) | b2n;
    int d  = kDist[(p[3] & 4) | ((p[3] & 1) << 1) | (v1 >= v2 ? 1 : 0)];
    int r1 = expand4((uint32_t)r1n), g1 = expand4((uint32_t)g1n), b1 = expand4((uint32_t)b1n);
    int r2 = expand4((uint32_t)r2n), g2 = expand4((uint32_t)g2n), b2 = expand4((uint32_t)b2n);
    const int paint[4][3] = {
        { clamp_u8(r1 + d), clamp_u8(g1 + d), clamp_u8(b1 + d) },
        { clamp_u8(r1 - d), clamp_u8(g1 - d), clamp_u8(b1 - d) },
        { clamp_u8(r2 + d), clamp_u8(g2 + d), clamp_u8(b2 + d) },
        { clamp_u8(r2 - d), clamp_u8(g2 - d), clamp_u8(b2 - d) },
    };
    paint_pixels(p, paint, opaque, rgba);
}

void decode_planar(const uint8_t *p, uint8_t rgba[64]) {
    int ro = (p[0] >> 1) & 0x3F;
    int go = ((p[0] & 1) << 6) | ((p[1] >> 1) & 0x3F);
    int bo = ((p[1] & 1) << 5) | (p[2] & 0x18) | ((p[2] & 3) << 1) | (p[3] >> 7);
    int rh = ((p[3] & 0x7C) >> 1) | (p[3] & 1);
    int gh = (p[4] >> 1) & 0x7F;
    int bh = ((p[4] & 1) << 5) | ((p[5] >> 3) & 0x1F);
    int rv = ((p[5] & 7) << 3) | ((p[6] >> 5) & 7);
    int gv = ((p[6] & 0x1F) << 2) | ((p[7] >> 6) & 3);
    int bv = p[7] & 0x3F;
    int O[3] = { e6(ro), e7(go), e6(bo) };
    int H[3] = { e6(rh), e7(gh), e6(bh) };
    int V[3] = { e6(rv), e7(gv), e6(bv) };
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint8_t *o = rgba + (y * 4 + x) * 4;
            for (int c = 0; c < 3; c++)
                o[c] = clamp_u8((x * (H[c] - O[c]) + y * (V[c] - O[c]) +
                                 4 * O[c] + 2) >> 2);
            o[3] = 255;                 /* planar is always opaque */
        }
    }
}

/* One 8-byte colour block. `punch` = punchthrough format (diff bit becomes
 * the opaque bit and the differential-family interpretation is forced). */
void decode_color_block(const uint8_t *p, bool punch, uint8_t rgba[64]) {
    bool diffbit = (p[3] & 2) != 0;

    if (!punch && !diffbit) {                        /* individual mode */
        int base[2][3] = {
            { expand4((uint32_t)(p[0] >> 4)), expand4((uint32_t)(p[1] >> 4)),
              expand4((uint32_t)(p[2] >> 4)) },
            { expand4((uint32_t)(p[0] & 15)), expand4((uint32_t)(p[1] & 15)),
              expand4((uint32_t)(p[2] & 15)) },
        };
        int table[2] = { p[3] >> 5, (p[3] >> 2) & 7 };
        decode_sub_indices(p, base, table, true, rgba);
        return;
    }

    bool opaque = punch ? diffbit : true;

    int r = p[0] >> 3, g = p[1] >> 3, b = p[2] >> 3;
    int r2 = r + sign3(p[0] & 7);
    int g2 = g + sign3(p[1] & 7);
    int b2 = b + sign3(p[2] & 7);

    if (r2 < 0 || r2 > 31) { decode_t_mode(p, opaque, rgba); return; }
    if (g2 < 0 || g2 > 31) { decode_h_mode(p, opaque, rgba); return; }
    if (b2 < 0 || b2 > 31) { decode_planar(p, rgba);         return; }

    int base[2][3] = {
        { expand5((uint32_t)r),  expand5((uint32_t)g),  expand5((uint32_t)b)  },
        { expand5((uint32_t)r2), expand5((uint32_t)g2), expand5((uint32_t)b2) },
    };
    int table[2] = { p[3] >> 5, (p[3] >> 2) & 7 };
    decode_sub_indices(p, base, table, opaque, rgba);
}

/* -------------------------------------------------------- EAC decoding --- */

/* 8-bit EAC alpha block; out[i] with i = x*4 + y. */
void decode_eac_alpha(const uint8_t *p, uint8_t out[16]) {
    int base = p[0];
    int mult = p[1] >> 4, tbl = p[1] & 15;
    uint64_t sel = load48be(p + 2);
    for (int i = 0; i < 16; i++) {
        int idx = (int)((sel >> (45 - 3 * i)) & 7);
        out[i] = clamp_u8(base + kAlphaMod[tbl][idx] * mult);
    }
}

/* 11-bit EAC channel block, mapped to 8 bits; out[i] with i = x*4 + y. */
void decode_eac_11(const uint8_t *p, bool snorm, uint8_t out[16]) {
    int mult = p[1] >> 4, tbl = p[1] & 15;
    int step = mult ? mult * 8 : 1;                  /* multiplier 0 -> *1 */
    uint64_t sel = load48be(p + 2);
    if (!snorm) {
        int base = p[0] * 8 + 4;
        for (int i = 0; i < 16; i++) {
            int idx = (int)((sel >> (45 - 3 * i)) & 7);
            int v = clampi(base + kAlphaMod[tbl][idx] * step, 0, 2047);
            out[i] = (uint8_t)((v * 255 + 1023) / 2047);
        }
    } else {
        int base = (int8_t)p[0];
        if (base == -128) base = -127;
        base *= 8;
        for (int i = 0; i < 16; i++) {
            int idx = (int)((sel >> (45 - 3 * i)) & 7);
            int v = clampi(base + kAlphaMod[tbl][idx] * step, -1023, 1023);
            out[i] = (uint8_t)(((v + 1023) * 255 + 1023) / 2046);
        }
    }
}

/* ------------------------------------------------------ colour encoding --- */

/* Pack 2-bit indices (idx[i], i = x*4+y) into the split msb/lsb planes. */
uint32_t pack_indices(const uint8_t idx[16]) {
    uint32_t lo = 0;
    for (int i = 0; i < 16; i++) {
        lo |= (uint32_t)(idx[i] & 1) << i;
        lo |= (uint32_t)((idx[i] >> 1) & 1) << (16 + i);
    }
    return lo;
}

void store_block(uint8_t *out, uint32_t lo) {
    out[4] = (uint8_t)(lo >> 24);
    out[5] = (uint8_t)(lo >> 16);
    out[6] = (uint8_t)(lo >> 8);
    out[7] = (uint8_t)lo;
}

/* ETC1 candidate: given flip + mode, quantise sub-block averages, do an
 * exhaustive table/index search, emit the block, return total sq error. */
uint64_t try_etc1(const uint8_t *px, bool flip, bool diffm, uint8_t out[8]) {
    int subof[16];
    double avg[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            int s = flip ? (y >> 1) : (x >> 1);
            subof[x * 4 + y] = s;
            const uint8_t *pp = px + (y * 4 + x) * 4;
            for (int c = 0; c < 3; c++) avg[s][c] += pp[c];
        }
    }
    for (int s = 0; s < 2; s++)
        for (int c = 0; c < 3; c++) avg[s][c] /= 8.0;

    int q[2][3], base[2][3];
    if (!diffm) {
        for (int s = 0; s < 2; s++)
            for (int c = 0; c < 3; c++) {
                q[s][c] = quant4(avg[s][c]);
                base[s][c] = expand4((uint32_t)q[s][c]);
            }
    } else {
        for (int c = 0; c < 3; c++) {
            q[0][c] = quant5(avg[0][c]);
            int d = clampi(quant5(avg[1][c]) - q[0][c], -4, 3);
            q[1][c] = q[0][c] + d;
            base[0][c] = expand5((uint32_t)q[0][c]);
            base[1][c] = expand5((uint32_t)q[1][c]);
        }
    }

    uint8_t idx[16] = { 0 };
    int bestT[2] = { 0, 0 };
    uint64_t toterr = 0;
    for (int s = 0; s < 2; s++) {
        uint64_t be = UINT64_MAX;
        int bt = 0;
        uint8_t bidx[16] = { 0 };
        for (int t = 0; t < 8; t++) {
            uint64_t e = 0;
            uint8_t ti[16] = { 0 };
            for (int x = 0; x < 4; x++) {
                for (int y = 0; y < 4; y++) {
                    int i = x * 4 + y;
                    if (subof[i] != s) continue;
                    const uint8_t *pp = px + (y * 4 + x) * 4;
                    uint64_t pb = UINT64_MAX;
                    int pk = 0;
                    for (int k = 0; k < 4; k++) {
                        int m = kMod[t][k];
                        uint64_t d = sqd3(pp, clamp_u8(base[s][0] + m),
                                              clamp_u8(base[s][1] + m),
                                              clamp_u8(base[s][2] + m));
                        if (d < pb) { pb = d; pk = k; }
                    }
                    ti[i] = (uint8_t)pk;
                    e += pb;
                }
            }
            if (e < be) { be = e; bt = t; memcpy(bidx, ti, 16); }
        }
        bestT[s] = bt;
        toterr += be;
        for (int i = 0; i < 16; i++)
            if (subof[i] == s) idx[i] = bidx[i];
    }

    if (!diffm) {
        out[0] = (uint8_t)((q[0][0] << 4) | q[1][0]);
        out[1] = (uint8_t)((q[0][1] << 4) | q[1][1]);
        out[2] = (uint8_t)((q[0][2] << 4) | q[1][2]);
    } else {
        out[0] = (uint8_t)((q[0][0] << 3) | ((q[1][0] - q[0][0]) & 7));
        out[1] = (uint8_t)((q[0][1] << 3) | ((q[1][1] - q[0][1]) & 7));
        out[2] = (uint8_t)((q[0][2] << 3) | ((q[1][2] - q[0][2]) & 7));
    }
    out[3] = (uint8_t)((bestT[0] << 5) | (bestT[1] << 2) |
                       (diffm ? 2 : 0) | (flip ? 1 : 0));
    store_block(out, pack_indices(idx));
    return toterr;
}

/* Pack a planar block. o/h/v are the quantised 6/7/6-bit corner colours.
 * The free bits are chosen so the mode detection lands on planar
 * (red and green diffs stay in range, blue diff overflows). */
void pack_planar(const int o[3], const int h[3], const int v[3], uint8_t p[8]) {
    int RO = o[0], GO = o[1], BO = o[2];
    int RH = h[0], GH = h[1], BH = h[2];
    int RV = v[0], GV = v[1], BV = v[2];

    p[0] = (uint8_t)((RO << 1) | (GO >> 6));
    p[1] = (uint8_t)(((GO & 0x3F) << 1) | (BO >> 5));
    p[2] = (uint8_t)((((BO >> 3) & 3) << 3) | ((BO >> 1) & 3));
    p[3] = (uint8_t)(((BO & 1) << 7) | (((RH >> 1) & 0x1F) << 2) | 2 | (RH & 1));
    p[4] = (uint8_t)((GH << 1) | (BH >> 5));
    p[5] = (uint8_t)(((BH & 0x1F) << 3) | (RV >> 3));
    p[6] = (uint8_t)(((RV & 7) << 5) | (GV >> 2));
    p[7] = (uint8_t)(((GV & 3) << 6) | BV);

    /* Red must NOT overflow: free bit 63 (p[0] bit 7). */
    for (int f = 0; f < 2; f++) {
        p[0] = (uint8_t)((p[0] & 0x7F) | (f << 7));
        int r = p[0] >> 3, dr = sign3(p[0] & 7);
        if (r + dr >= 0 && r + dr <= 31) break;
    }
    /* Green must NOT overflow: free bit 55 (p[1] bit 7). */
    for (int f = 0; f < 2; f++) {
        p[1] = (uint8_t)((p[1] & 0x7F) | (f << 7));
        int g = p[1] >> 3, dg = sign3(p[1] & 7);
        if (g + dg >= 0 && g + dg <= 31) break;
    }
    /* Blue MUST overflow: free bits 47..45 and 42 (p[2] bits 7..5, 2). */
    for (int f2 = 0; f2 < 8; f2++) {
        bool done = false;
        for (int f3 = 0; f3 < 2; f3++) {
            p[2] = (uint8_t)((p[2] & 0x1B) | (f2 << 5) | (f3 << 2));
            int b = p[2] >> 3, db = sign3(p[2] & 7);
            if (b + db < 0 || b + db > 31) { done = true; break; }
        }
        if (done) break;
    }
}

/* Planar candidate: least-squares plane fit per channel. */
uint64_t try_planar(const uint8_t *px, uint8_t out[8]) {
    int o[3], h[3], v[3];
    for (int c = 0; c < 3; c++) {
        double sum = 0, sx = 0, sy = 0;
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                double vv = px[(y * 4 + x) * 4 + c];
                sum += vv;
                sx += (x - 1.5) * vv;
                sy += (y - 1.5) * vv;
            }
        }
        double bc = sx / 20.0, cc = sy / 20.0;       /* sum (x-1.5)^2 = 20 */
        double a = sum / 16.0 - 1.5 * bc - 1.5 * cc;
        double O = a, H = a + 4.0 * bc, V = a + 4.0 * cc;
        int bits = (c == 1) ? 127 : 63;
        double sc = bits / 255.0;
        o[c] = clampi((int)std::lround(O * sc), 0, bits);
        h[c] = clampi((int)std::lround(H * sc), 0, bits);
        v[c] = clampi((int)std::lround(V * sc), 0, bits);
    }
    pack_planar(o, h, v, out);
    uint8_t tmp[64];
    decode_planar(out, tmp);
    uint64_t err = 0;
    for (int i = 0; i < 16; i++)
        err += sqd3(px + i * 4, tmp[i * 4], tmp[i * 4 + 1], tmp[i * 4 + 2]);
    return err;
}

/* Punchthrough candidate (opaque bit = 0, differential layout).
 * Transparent pixels take index 2; opaque pixels pick among {0, +b, -b}. */
uint64_t try_punch(const uint8_t *px, bool flip, uint8_t out[8],
                   int alpha_thresh) {
    int subof[16];
    double avg[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
    int cnt[2] = { 0, 0 };
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            int s = flip ? (y >> 1) : (x >> 1);
            subof[x * 4 + y] = s;
            const uint8_t *pp = px + (y * 4 + x) * 4;
            if (pp[3] >= 128) {
                cnt[s]++;
                for (int c = 0; c < 3; c++) avg[s][c] += pp[c];
            }
        }
    }
    for (int s = 0; s < 2; s++)
        if (cnt[s])
            for (int c = 0; c < 3; c++) avg[s][c] /= cnt[s];

    int q[2][3], base[2][3];
    for (int c = 0; c < 3; c++) {
        q[0][c] = quant5(avg[0][c]);
        int d = clampi(quant5(avg[1][c]) - q[0][c], -4, 3);
        q[1][c] = q[0][c] + d;
        base[0][c] = expand5((uint32_t)q[0][c]);
        base[1][c] = expand5((uint32_t)q[1][c]);
    }

    static const int kAllowed[3] = { 0, 1, 3 };      /* idx 2 = transparent */
    uint8_t idx[16] = { 0 };
    int bestT[2] = { 0, 0 };
    uint64_t toterr = 0;
    for (int s = 0; s < 2; s++) {
        uint64_t be = UINT64_MAX;
        int bt = 0;
        uint8_t bidx[16] = { 0 };
        for (int t = 0; t < 8; t++) {
            uint64_t e = 0;
            uint8_t ti[16] = { 0 };
            for (int x = 0; x < 4; x++) {
                for (int y = 0; y < 4; y++) {
                    int i = x * 4 + y;
                    if (subof[i] != s) continue;
                    const uint8_t *pp = px + (y * 4 + x) * 4;
                    if (pp[3] < alpha_thresh) { ti[i] = 2; continue; }
                    uint64_t pb = UINT64_MAX;
                    int pk = 0;
                    for (int a = 0; a < 3; a++) {
                        int k = kAllowed[a];
                        int m = (k == 0) ? 0 : kMod[t][k];
                        uint64_t d = sqd3(pp, clamp_u8(base[s][0] + m),
                                              clamp_u8(base[s][1] + m),
                                              clamp_u8(base[s][2] + m));
                        if (d < pb) { pb = d; pk = k; }
                    }
                    ti[i] = (uint8_t)pk;
                    e += pb;
                }
            }
            if (e < be) { be = e; bt = t; memcpy(bidx, ti, 16); }
        }
        bestT[s] = bt;
        toterr += be;
        for (int i = 0; i < 16; i++)
            if (subof[i] == s) idx[i] = bidx[i];
    }

    out[0] = (uint8_t)((q[0][0] << 3) | ((q[1][0] - q[0][0]) & 7));
    out[1] = (uint8_t)((q[0][1] << 3) | ((q[1][1] - q[0][1]) & 7));
    out[2] = (uint8_t)((q[0][2] << 3) | ((q[1][2] - q[0][2]) & 7));
    out[3] = (uint8_t)((bestT[0] << 5) | (bestT[1] << 2) | (flip ? 1 : 0));
    store_block(out, pack_indices(idx));             /* opaque bit stays 0 */
    return toterr;
}

/* Best ETC1(/ETC2) colour block. allow_individual=false for punchthrough
 * opaque blocks (the diff bit is the opaque bit there). */
void encode_color_block(const uint8_t *px, bool allow_individual,
                        bool allow_planar, uint8_t out[8]) {
    uint8_t cand[8];
    uint64_t best = UINT64_MAX;
    for (int flip = 0; flip < 2; flip++) {
        for (int dm = 0; dm < 2; dm++) {
            if (dm == 0 && !allow_individual) continue;
            uint64_t e = try_etc1(px, flip != 0, dm != 0, cand);
            if (e < best) { best = e; memcpy(out, cand, 8); }
        }
    }
    if (allow_planar) {
        uint64_t e = try_planar(px, cand);
        if (e < best) { best = e; memcpy(out, cand, 8); }
    }
}

void encode_rgba1_block(const uint8_t *px, uint8_t out[8], int alpha_thresh) {
    bool any_t = false;
    for (int i = 0; i < 16; i++)
        if (px[i * 4 + 3] < alpha_thresh) { any_t = true; break; }

    uint8_t cand[8];
    uint64_t best = UINT64_MAX;
    for (int flip = 0; flip < 2; flip++) {
        uint64_t e = any_t ? try_punch(px, flip != 0, cand, alpha_thresh)
                           : try_etc1(px, flip != 0, true, cand);
        if (e < best) { best = e; memcpy(out, cand, 8); }
    }
}

/* --------------------------------------------------------- EAC encoding --- */

void store_selectors(uint8_t *out, const uint8_t idx[16]) {
    uint64_t sel = 0;
    for (int i = 0; i < 16; i++)
        sel |= (uint64_t)(idx[i] & 7) << (45 - 3 * i);
    for (int i = 0; i < 6; i++)
        out[2 + i] = (uint8_t)(sel >> (40 - 8 * i));
}

/* 8-bit EAC alpha block. a[i] with i = x*4 + y. */
void encode_eac_alpha(const uint8_t a[16], uint8_t out[8]) {
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) {
        if (a[i] < amin) amin = a[i];
        if (a[i] > amax) amax = a[i];
    }
    double center = (amin + amax) / 2.0;

    uint64_t best = UINT64_MAX;
    int bb = 128, bm = 1, bt = 0;
    uint8_t bidx[16] = { 0 };
    for (int t = 0; t < 16; t++) {
        for (int m = 1; m <= 15; m++) {
            int bc = (int)std::lround(
                center - m * (kAlphaMod[t][3] + kAlphaMod[t][7]) / 2.0);
            for (int off = -1; off <= 1; off++) {
                int base = clampi(bc + off, 0, 255);
                uint64_t e = 0;
                uint8_t ti[16];
                for (int i = 0; i < 16; i++) {
                    uint64_t pb = UINT64_MAX;
                    int pk = 0;
                    for (int k = 0; k < 8; k++) {
                        int d = clamp_u8(base + kAlphaMod[t][k] * m) - a[i];
                        uint64_t dd = (uint64_t)(d * d);
                        if (dd < pb) { pb = dd; pk = k; }
                    }
                    ti[i] = (uint8_t)pk;
                    e += pb;
                }
                if (e < best) {
                    best = e; bb = base; bm = m; bt = t;
                    memcpy(bidx, ti, 16);
                }
            }
        }
    }
    out[0] = (uint8_t)bb;
    out[1] = (uint8_t)((bm << 4) | bt);
    store_selectors(out, bidx);
}

/* 11-bit EAC channel block from 8-bit input. a[i] with i = x*4 + y. */
void encode_eac_11(const uint8_t a[16], bool snorm, uint8_t out[8]) {
    int tgt[16];
    int tmin = 4096, tmax = -4096;
    for (int i = 0; i < 16; i++) {
        tgt[i] = snorm ? (int)std::lround(a[i] * 2046.0 / 255.0) - 1023
                       : (int)std::lround(a[i] * 2047.0 / 255.0);
        if (tgt[i] < tmin) tmin = tgt[i];
        if (tgt[i] > tmax) tmax = tgt[i];
    }
    double center = (tmin + tmax) / 2.0;

    uint64_t best = UINT64_MAX;
    int bb = 0, bm = 1, bt = 0;
    uint8_t bidx[16] = { 0 };
    for (int t = 0; t < 16; t++) {
        for (int m = 0; m <= 15; m++) {
            int step = m ? m * 8 : 1;
            double basef = (center - (snorm ? 0.0 : 4.0) -
                            step * (kAlphaMod[t][3] + kAlphaMod[t][7]) / 2.0) / 8.0;
            int bc = (int)std::lround(basef);
            for (int off = -1; off <= 1; off++) {
                int base = snorm ? clampi(bc + off, -127, 127)
                                 : clampi(bc + off, 0, 255);
                int b8 = snorm ? base * 8 : base * 8 + 4;
                uint64_t e = 0;
                uint8_t ti[16];
                for (int i = 0; i < 16; i++) {
                    uint64_t pb = UINT64_MAX;
                    int pk = 0;
                    for (int k = 0; k < 8; k++) {
                        int v = b8 + kAlphaMod[t][k] * step;
                        v = snorm ? clampi(v, -1023, 1023) : clampi(v, 0, 2047);
                        int d = v - tgt[i];
                        uint64_t dd = (uint64_t)((int64_t)d * d);
                        if (dd < pb) { pb = dd; pk = k; }
                    }
                    ti[i] = (uint8_t)pk;
                    e += pb;
                }
                if (e < best) {
                    best = e; bb = base; bm = m; bt = t;
                    memcpy(bidx, ti, 16);
                }
            }
        }
    }
    out[0] = snorm ? (uint8_t)(int8_t)bb : (uint8_t)bb;
    out[1] = (uint8_t)((bm << 4) | bt);
    store_selectors(out, bidx);
}

/* ------------------------------------------------------------- utility --- */

size_t etc_block_bytes(texc_format fmt) {
    switch (fmt) {
    case TEXC_FORMAT_ETC2_RGBA8:
    case TEXC_FORMAT_EAC_RG11:
    case TEXC_FORMAT_EAC_RG11_SIGNED:
        return 16;
    default:
        return 8;
    }
}

} /* anonymous namespace */

/* ------------------------------------------------------------ dispatch --- */

int etc_decode(texc_format fmt, const uint8_t *src, size_t src_size,
               uint32_t width, uint32_t height, uint8_t *dst) {
    (void)src_size;
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    size_t bs = etc_block_bytes(fmt);
    uint8_t tmp[64];
    uint8_t ch[16], ch2[16];

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *p = src + ((size_t)by * bw + bx) * bs;
            switch (fmt) {
            case TEXC_FORMAT_ETC1_RGB:
            case TEXC_FORMAT_ETC2_RGB:
                decode_color_block(p, false, tmp);
                break;
            case TEXC_FORMAT_ETC2_RGBA1:
                decode_color_block(p, true, tmp);
                break;
            case TEXC_FORMAT_ETC2_RGBA8:
                decode_color_block(p + 8, false, tmp);
                decode_eac_alpha(p, ch);
                for (int x = 0; x < 4; x++)
                    for (int y = 0; y < 4; y++)
                        tmp[(y * 4 + x) * 4 + 3] = ch[x * 4 + y];
                break;
            case TEXC_FORMAT_EAC_R11:
            case TEXC_FORMAT_EAC_R11_SIGNED:
                decode_eac_11(p, fmt == TEXC_FORMAT_EAC_R11_SIGNED, ch);
                for (int x = 0; x < 4; x++)
                    for (int y = 0; y < 4; y++) {
                        uint8_t *o = tmp + (y * 4 + x) * 4;
                        o[0] = ch[x * 4 + y];
                        o[1] = 0; o[2] = 0; o[3] = 255;
                    }
                break;
            case TEXC_FORMAT_EAC_RG11:
            case TEXC_FORMAT_EAC_RG11_SIGNED: {
                bool sn = fmt == TEXC_FORMAT_EAC_RG11_SIGNED;
                decode_eac_11(p, sn, ch);
                decode_eac_11(p + 8, sn, ch2);
                for (int x = 0; x < 4; x++)
                    for (int y = 0; y < 4; y++) {
                        uint8_t *o = tmp + (y * 4 + x) * 4;
                        o[0] = ch[x * 4 + y];
                        o[1] = ch2[x * 4 + y];
                        o[2] = 0; o[3] = 255;
                    }
                break;
            }
            default:
                return TEXC_ERR_UNSUPPORTED;
            }
            write_block_rgba8(dst, width, height, bx, by, 4, 4, tmp);
        }
    }
    return TEXC_OK;
}

int etc_encode(texc_format fmt, const uint8_t *src,
               uint32_t width, uint32_t height, uint8_t *dst,
               const texc_encode_options *opts) {
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    size_t bs = etc_block_bytes(fmt);
    uint8_t px[64];
    uint8_t ch[16];
    int alpha_thresh = (int)(opts ? opts->alpha_threshold : 128u);
    if (alpha_thresh > 256) alpha_thresh = 256;

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            read_block_rgba8(src, width, height, bx, by, 4, 4, px);
            uint8_t *o = dst + ((size_t)by * bw + bx) * bs;
            switch (fmt) {
            case TEXC_FORMAT_ETC1_RGB:
                encode_color_block(px, true, false, o);
                break;
            case TEXC_FORMAT_ETC2_RGB:
                encode_color_block(px, true, true, o);
                break;
            case TEXC_FORMAT_ETC2_RGBA1:
                encode_rgba1_block(px, o, alpha_thresh);
                break;
            case TEXC_FORMAT_ETC2_RGBA8:
                for (int x = 0; x < 4; x++)
                    for (int y = 0; y < 4; y++)
                        ch[x * 4 + y] = px[(y * 4 + x) * 4 + 3];
                encode_eac_alpha(ch, o);
                encode_color_block(px, true, true, o + 8);
                break;
            case TEXC_FORMAT_EAC_R11:
            case TEXC_FORMAT_EAC_R11_SIGNED:
                for (int x = 0; x < 4; x++)
                    for (int y = 0; y < 4; y++)
                        ch[x * 4 + y] = px[(y * 4 + x) * 4];
                encode_eac_11(ch, fmt == TEXC_FORMAT_EAC_R11_SIGNED, o);
                break;
            case TEXC_FORMAT_EAC_RG11:
            case TEXC_FORMAT_EAC_RG11_SIGNED: {
                bool sn = fmt == TEXC_FORMAT_EAC_RG11_SIGNED;
                for (int c = 0; c < 2; c++) {
                    for (int x = 0; x < 4; x++)
                        for (int y = 0; y < 4; y++)
                            ch[x * 4 + y] = px[(y * 4 + x) * 4 + c];
                    encode_eac_11(ch, sn, o + c * 8);
                }
                break;
            }
            default:
                return TEXC_ERR_UNSUPPORTED;
            }
        }
    }
    return TEXC_OK;
}

} /* namespace texc */

/* ============================================================ self-test === */
#ifdef TEXC_SELFTEST

#include <cstdio>
#include <vector>

using namespace texc;

static int g_failures = 0;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            std::printf("FAIL: " __VA_ARGS__);                               \
            std::printf("\n");                                               \
        }                                                                    \
    } while (0)

static uint32_t g_rng;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static size_t enc_size(texc_format fmt, uint32_t w, uint32_t h) {
    size_t bs = (fmt == TEXC_FORMAT_ETC2_RGBA8 || fmt == TEXC_FORMAT_EAC_RG11 ||
                 fmt == TEXC_FORMAT_EAC_RG11_SIGNED) ? 16 : 8;
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * bs;
}

static double psnr(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
                   uint32_t w, uint32_t h, uint32_t chmask) {
    double mse = 0;
    size_t n = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        for (int c = 0; c < 4; c++) {
            if (!(chmask & (1u << c))) continue;
            double d = (double)a[i * 4 + c] - (double)b[i * 4 + c];
            mse += d * d;
            n++;
        }
    }
    mse /= (double)n;
    if (mse <= 0.0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

static double roundtrip(texc_format fmt, const std::vector<uint8_t> &img,
                        uint32_t w, uint32_t h, uint32_t chmask) {
    std::vector<uint8_t> enc(enc_size(fmt, w, h));
    std::vector<uint8_t> dec((size_t)w * h * 4, 0);
    int r1 = etc_encode(fmt, img.data(), w, h, enc.data(), nullptr);
    int r2 = etc_decode(fmt, enc.data(), enc.size(), w, h, dec.data());
    CHECK(r1 == TEXC_OK && r2 == TEXC_OK, "roundtrip result fmt=%d", (int)fmt);
    return psnr(img, dec, w, h, chmask);
}

static void make_gradient(std::vector<uint8_t> &img, uint32_t w, uint32_t h,
                          bool alpha_grad) {
    img.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = &img[((size_t)y * w + x) * 4];
            int r = w > 1 ? (int)(x * 255 / (w - 1)) : 0;
            int g = h > 1 ? (int)(y * 255 / (h - 1)) : 0;
            p[0] = (uint8_t)r;
            p[1] = (uint8_t)g;
            p[2] = (uint8_t)((x + y) * 255 / (w + h - 2));
            p[3] = alpha_grad ? (uint8_t)(255 - r) : 255;
        }
    }
}

static void make_random(std::vector<uint8_t> &img, uint32_t w, uint32_t h,
                        bool opaque) {
    img.resize((size_t)w * h * 4);
    for (size_t i = 0; i < img.size(); i++) img[i] = (uint8_t)(rnd() & 255);
    if (opaque)
        for (size_t i = 3; i < img.size(); i += 4) img[i] = 255;
}

static void kat_etc1(void) {
    /* Individual mode, both sub-block colours 4-bit (8,8,8), tables 0/0,
     * flip 0, all pixel indices 0 except pixel i=1 (x=0,y=1) with lsb set.
     * expand4(8) = 136; table 0: idx0 -> +2 (138), idx1 -> +8 (144). */
    const uint8_t blk[8] = { 0x88, 0x88, 0x88, 0x00, 0, 0, 0, 0x02 };
    std::vector<uint8_t> out(64, 0);
    CHECK(etc_decode(TEXC_FORMAT_ETC1_RGB, blk, 8, 4, 4, out.data()) == TEXC_OK,
          "etc1 KAT decode");
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t *p = &out[(y * 4 + x) * 4];
            int want = (x == 0 && y == 1) ? 144 : 138;
            CHECK(p[0] == want && p[1] == want && p[2] == want && p[3] == 255,
                  "etc1 KAT pixel (%d,%d) got %d,%d,%d,%d want %d", x, y,
                  p[0], p[1], p[2], p[3], want);
        }
    }
}

static void kat_t_mode(void) {
    /* byte0=0x07: R=0, dR=-1 -> overflow -> T mode.
     * T fields: R1=3, G1=2, B1=5; R2=8, G2=12, B2=4; da=2, db=0 -> dist 4
     * -> d=23. sel=0 -> every pixel = paint0 = base1 = (51,34,85). */
    const uint8_t blk[8] = { 0x07, 0x25, 0x8C, 0x4A, 0, 0, 0, 0 };
    std::vector<uint8_t> out(64, 0);
    etc_decode(TEXC_FORMAT_ETC2_RGB, blk, 8, 4, 4, out.data());
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = &out[i * 4];
        CHECK(p[0] == 51 && p[1] == 34 && p[2] == 85 && p[3] == 255,
              "T-mode KAT pixel %d got %d,%d,%d", i, p[0], p[1], p[2]);
    }
}

static void kat_h_mode(void) {
    /* byte0=0x00 (R ok), byte1=0x04: G=0, dG=-4 -> overflow -> H mode.
     * base1=(0,0,17), base2=(255,238,0), v1<v2, byte3=0x02 -> dist 0 -> d=3.
     * sel=0 -> every pixel = paint0 = base1+3 = (3,3,20). */
    const uint8_t blk[8] = { 0x00, 0x04, 0xFF, 0x02, 0, 0, 0, 0 };
    std::vector<uint8_t> out(64, 0);
    etc_decode(TEXC_FORMAT_ETC2_RGB, blk, 8, 4, 4, out.data());
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = &out[i * 4];
        CHECK(p[0] == 3 && p[1] == 3 && p[2] == 20 && p[3] == 255,
              "H-mode KAT pixel %d got %d,%d,%d", i, p[0], p[1], p[2]);
    }
}

static void kat_planar(void) {
    /* Hand-built planar block, O=H=V=(63,127,63) -> all white. */
    const uint8_t blk[8] = { 0x7F, 0x7F, 0xFB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    std::vector<uint8_t> out(64, 0);
    etc_decode(TEXC_FORMAT_ETC2_RGB, blk, 8, 4, 4, out.data());
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = &out[i * 4];
        CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255,
              "planar KAT pixel %d got %d,%d,%d", i, p[0], p[1], p[2]);
    }
}

static void kat_eac_r11_mult0(void) {
    /* base=100, multiplier=0, table 0, all indices 0:
     * v = 100*8+4 + (-3)*1 = 801 -> (801*255+1023)/2047 = 100. */
    const uint8_t blk[8] = { 100, 0x00, 0, 0, 0, 0, 0, 0 };
    std::vector<uint8_t> out(64, 0);
    etc_decode(TEXC_FORMAT_EAC_R11, blk, 8, 4, 4, out.data());
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = &out[i * 4];
        CHECK(p[0] == 100 && p[1] == 0 && p[2] == 0 && p[3] == 255,
              "EAC R11 mult0 KAT pixel %d got %d,%d,%d,%d", i,
              p[0], p[1], p[2], p[3]);
    }
}

static void test_punchthrough(void) {
    /* 8x8: left half transparent, right half opaque (200,50,100). */
    uint32_t w = 8, h = 8;
    std::vector<uint8_t> img((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = &img[((size_t)y * w + x) * 4];
            if (x < 4) {
                p[0] = (uint8_t)(rnd() & 255);
                p[1] = (uint8_t)(rnd() & 255);
                p[2] = (uint8_t)(rnd() & 255);
                p[3] = 0;
            } else {
                p[0] = 200; p[1] = 50; p[2] = 100; p[3] = 255;
            }
        }
    }
    std::vector<uint8_t> enc(enc_size(TEXC_FORMAT_ETC2_RGBA1, w, h));
    std::vector<uint8_t> dec((size_t)w * h * 4, 1);
    etc_encode(TEXC_FORMAT_ETC2_RGBA1, img.data(), w, h, enc.data(), nullptr);
    etc_decode(TEXC_FORMAT_ETC2_RGBA1, enc.data(), enc.size(), w, h, dec.data());
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = &dec[((size_t)y * w + x) * 4];
            if (x < 4) {
                CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0,
                      "punchthrough hole (%u,%u) got %d,%d,%d,%d", x, y,
                      p[0], p[1], p[2], p[3]);
            } else {
                CHECK(p[3] == 255, "punchthrough opaque (%u,%u) alpha %d",
                      x, y, p[3]);
            }
        }
    }
}

int main(void) {
    struct FmtTest {
        texc_format fmt;
        const char *name;
        uint32_t mask;       /* channels compared */
        double gthresh;      /* required gradient PSNR */
        bool alpha_grad;     /* gradient carries an alpha ramp */
        bool opaque_random;  /* force alpha=255 in random test  */
    };
    const FmtTest tests[] = {
        { TEXC_FORMAT_ETC1_RGB,        "ETC1_RGB",        0x7, 28, false, true  },
        { TEXC_FORMAT_ETC2_RGB,        "ETC2_RGB",        0x7, 28, false, true  },
        { TEXC_FORMAT_ETC2_RGBA1,      "ETC2_RGBA1",      0x7, 28, false, true  },
        { TEXC_FORMAT_ETC2_RGBA8,      "ETC2_RGBA8",      0xF, 28, true,  false },
        { TEXC_FORMAT_EAC_R11,         "EAC_R11",         0x1, 35, false, false },
        { TEXC_FORMAT_EAC_R11_SIGNED,  "EAC_R11_SIGNED",  0x1, 35, false, false },
        { TEXC_FORMAT_EAC_RG11,        "EAC_RG11",        0x3, 35, false, false },
        { TEXC_FORMAT_EAC_RG11_SIGNED, "EAC_RG11_SIGNED", 0x3, 35, false, false },
    };
    const uint32_t sizes[][2] = { { 16, 16 }, { 37, 23 } };

    kat_etc1();
    kat_t_mode();
    kat_h_mode();
    kat_planar();
    kat_eac_r11_mult0();
    test_punchthrough();

    std::vector<uint8_t> img;
    for (const FmtTest &t : tests) {
        for (const uint32_t *sz : sizes) {
            uint32_t w = sz[0], h = sz[1];
            make_gradient(img, w, h, t.alpha_grad);
            double gp = roundtrip(t.fmt, img, w, h, t.mask);
            g_rng = 12345;
            make_random(img, w, h, t.opaque_random);
            double rp = roundtrip(t.fmt, img, w, h, t.mask);
            std::printf("%-16s %3ux%-3u gradient %6.2f dB  random %6.2f dB\n",
                        t.name, w, h, gp, rp);
            CHECK(gp >= t.gthresh, "%s %ux%u gradient PSNR %.2f < %.2f",
                  t.name, w, h, gp, t.gthresh);
        }
    }

    if (g_failures) {
        std::printf("SELFTEST FAILED (%d failures)\n", g_failures);
        return 1;
    }
    std::printf("SELFTEST PASSED\n");
    return 0;
}

#endif /* TEXC_SELFTEST */
