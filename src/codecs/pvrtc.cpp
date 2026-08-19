/*
 * pvrtc.cpp - PVRTC1 / PVRTC2 codec module (see codec_common.h).
 *
 * PVRTC1 (2bpp/4bpp, RGB/RGBA):
 *   The decoder implements the format as documented in the Khronos Data
 *   Format Specification's PVRTC appendix (derived from the PVRTC Texture
 *   Compression User Guide / Fenney, "Texture Compression using
 *   Low-Frequency Signal Modulation", GH2003) and matches the PowerVR SDK
 *   PVRTDecompress reference arithmetic:
 *     - 64-bit little-endian words: bits 31..0 modulation data,
 *       bits 63..32 colour data (bit31 = colour-B opaque flag,
 *       bits 30..16 colour B, bit15 = colour-A opaque flag,
 *       bits 14..1 colour A, bit0 = modulation mode flag M),
 *     - colours parsed into 5-bit RGB + 4-bit A (opaque: B=555, A=554;
 *       translucent: B=3444, A=3443 with the 3-bit alpha zero-padded to
 *       4 bits), lower-precision channels bit-replicated to 5 bits,
 *     - the two low-resolution colour images are bilinearly upscaled x4
 *       (x8 horizontally for 2bpp) with toroidal WRAP addressing and
 *       converted to 8888 with the spec's fixed-point rules
 *       (RGB: C/2 + C/64 for 4bpp, C/4 + C/128 for 2bpp;
 *        A:   C + C/16  for 4bpp, C/2 + C/32  for 2bpp),
 *     - 4bpp modulation: M=0 -> weights {0,3,5,8}; M=1 -> {0,4,4,8} with
 *       value 2 punching alpha through to 0,
 *     - 2bpp modulation: M=0 -> direct 1 bit per texel (A or B); M=1 ->
 *       checkerboard-stored 2-bit values with H&V / H-only / V-only
 *       neighbour-interpolation submodes (modulation bit0 selects H/V-only,
 *       bit20 selects V, bits 1 and 21 stand in for the repurposed bits of
 *       texels (0,0) and (4,2)),
 *     - words stored in reflected Morton order over a word grid padded to a
 *       minimum of 2x2 (matching texc_encoded_size); padding words replicate
 *       the adjacent logical words as the spec recommends,
 *     - image dimensions must be powers of two and at least one block.
 *
 * PVRTC2 (2bpp/4bpp):
 *   Implemented per the same Khronos description: words in linear row-major
 *   order, any image size >= 1 (block-padded storage), a single opacity
 *   flag in bit31 covering both colours (translucent colour B's 3-bit alpha
 *   expands with a low 1 bit instead of 0), hard-transition flag H in bit15.
 *   4bpp modes: standard bilinear (H=0,M=0), punch-through (H=0,M=1, texel
 *   value 2 -> transparent BLACK), non-interpolated (H=1,M=0: the texel's
 *   own word's colours, expanded 4555 -> 8888 by replication) and local
 *   palette (H=1,M=1: per-texel palette from the four region words per the
 *   spec's mapping table). 2bpp: H selects bilinear vs non-interpolated
 *   colours, M selects direct vs interpolated modulation as in PVRTC1.
 *   Details not pinned down by the public documentation (noted limitations):
 *   colour reconstruction samples CLAMP at image borders instead of
 *   wrapping, and 2bpp interpolated-modulation neighbours wrap within the
 *   block-padded modulation lattice.
 *
 * Encoders:
 *   PVRTC1: classic low-frequency encoder - per-word component-wise
 *   min/max endpoint images, quantized, then per-texel best modulation
 *   evaluated against the exact decoder reconstruction (4bpp: direct 2-bit
 *   weights; 2bpp: checkerboard-interpolated submode H&V).
 *   PVRTC2: every word uses the hard-transition, non-interpolated mode so
 *   blocks are self-contained: per-block min/max endpoints and best
 *   per-texel modulation (4bpp: M=0 2-bit; 2bpp: M=1 checkerboard).
 */

#include "codec_common.h"

#include <vector>

namespace texc {
namespace {

/* ------------------------------------------------------------- helpers --- */

inline uint32_t load32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void store32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

inline bool ispow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

/* floor division for b > 0 */
inline int floordiv(int a, int b) {
    int q = a / b;
    if ((a % b) != 0 && a < 0) q--;
    return q;
}
inline int wrapi(int v, int n) {
    v %= n;
    return v < 0 ? v + n : v;
}
inline int clampidx(int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }

/* Reflected Morton order word offset (Khronos Data Format spec, PVRTC1).
 * Y contributes the lower bit of each interleaved pair; leftover high bits
 * of the larger dimension are appended. Dimensions must be powers of two. */
uint32_t morton_offset(uint32_t X, uint32_t Y, uint32_t wordsW, uint32_t wordsH) {
    uint32_t minDim = (wordsW <= wordsH) ? wordsW : wordsH;
    uint32_t offset = 0, shift = 0, mask;
    for (mask = 1; mask < minDim; mask <<= 1) {
        offset |= ((Y & mask) | ((X & mask) << 1)) << shift;
        shift++;
    }
    offset |= ((X | Y) >> shift) << (shift * 2);
    return offset;
}

/* ------------------------------------------------------------- colours --- */

struct C5 { int r, g, b, a; };   /* 5-bit RGB, 4-bit A */
struct Px { int r, g, b, a; };   /* 8888 */

/* Colour A: bits 14..1 of the colour word (+ opaque flag). For PVRTC1 the
 * opaque flag is bit15; for PVRTC2 the single opacity flag is bit31. */
C5 parse_colorA(uint32_t cw, bool pvrtc2) {
    bool opaque = pvrtc2 ? ((cw >> 31) & 1) != 0 : ((cw >> 15) & 1) != 0;
    C5 c;
    if (opaque) {                            /* RGB 554 */
        c.r = (int)((cw >> 10) & 31);
        c.g = (int)((cw >> 5) & 31);
        int b4 = (int)((cw >> 1) & 15);
        c.b = (b4 << 1) | (b4 >> 3);
        c.a = 15;
    } else {                                 /* ARGB 3443 */
        int r4 = (int)((cw >> 8) & 15), g4 = (int)((cw >> 4) & 15);
        int b3 = (int)((cw >> 1) & 7);
        c.r = (r4 << 1) | (r4 >> 3);
        c.g = (g4 << 1) | (g4 >> 3);
        c.b = (b3 << 2) | (b3 >> 1);
        c.a = (int)((cw >> 12) & 7) << 1;    /* zero-padded to 4 bits */
    }
    return c;
}

/* Colour B: bits 30..16 (+ opaque flag in bit31). In PVRTC2 translucent
 * mode, B's 3-bit alpha expands with a low 1 bit (A2A1A0 1). */
C5 parse_colorB(uint32_t cw, bool pvrtc2) {
    bool opaque = ((cw >> 31) & 1) != 0;
    C5 c;
    if (opaque) {                            /* RGB 555 */
        c.r = (int)((cw >> 26) & 31);
        c.g = (int)((cw >> 21) & 31);
        c.b = (int)((cw >> 16) & 31);
        c.a = 15;
    } else {                                 /* ARGB 3444 */
        int r4 = (int)((cw >> 24) & 15), g4 = (int)((cw >> 20) & 15);
        int b4 = (int)((cw >> 16) & 15);
        c.r = (r4 << 1) | (r4 >> 3);
        c.g = (g4 << 1) | (g4 >> 3);
        c.b = (b4 << 1) | (b4 >> 3);
        c.a = ((int)((cw >> 28) & 7) << 1) | (pvrtc2 ? 1 : 0);
    }
    return c;
}

/* Second bit-replication step: 4555 -> 8888. */
inline Px c5_to_px(const C5 &c) {
    Px p;
    p.r = (c.r << 3) | (c.r >> 2);
    p.g = (c.g << 3) | (c.g >> 2);
    p.b = (c.b << 3) | (c.b >> 2);
    p.a = c.a * 17;
    return p;
}

/* ----------------------------------------------------------- word grid --- */

/* Logical row-major grid of decoded 32-bit modulation/colour words. */
struct Grid {
    std::vector<uint32_t> col, mod;
    int wx = 0, wy = 0;   /* logical word counts */
    int bw = 4;           /* block width in texels (4 or 8); height is 4 */
    bool wrap = true;     /* PVRTC1 wraps word indices, PVRTC2 clamps */
    bool pvrtc2 = false;

    int fixx(int v) const { return wrap ? wrapi(v, wx) : clampidx(v, wx); }
    int fixy(int v) const { return wrap ? wrapi(v, wy) : clampidx(v, wy); }
    uint32_t colw(int X, int Y) const { return col[(size_t)Y * wx + X]; }
    uint32_t modw(int X, int Y) const { return mod[(size_t)Y * wx + X]; }
};

/* Bilinear upscale of the A/B low-resolution images at texel (x, y),
 * following the spec's integer arithmetic exactly. Coordinates may lie in
 * the block-padded texel space. */
void upscale(const Grid &g, int x, int y, Px &A, Px &B) {
    const int bw = g.bw;
    int XL = floordiv(x - bw / 2, bw), YL = floordiv(y - 2, 4);
    int xr = (x - bw / 2) - bw * XL, yr = (y - 2) - 4 * YL;
    int X0 = g.fixx(XL), X1 = g.fixx(XL + 1);
    int Y0 = g.fixy(YL), Y1 = g.fixy(YL + 1);
    uint32_t w00 = g.colw(X0, Y0), w10 = g.colw(X1, Y0);
    uint32_t w01 = g.colw(X0, Y1), w11 = g.colw(X1, Y1);
    int k00 = (bw - xr) * (4 - yr), k10 = xr * (4 - yr);
    int k01 = (bw - xr) * yr, k11 = xr * yr;

    for (int pass = 0; pass < 2; pass++) {
        C5 c00 = pass ? parse_colorB(w00, g.pvrtc2) : parse_colorA(w00, g.pvrtc2);
        C5 c10 = pass ? parse_colorB(w10, g.pvrtc2) : parse_colorA(w10, g.pvrtc2);
        C5 c01 = pass ? parse_colorB(w01, g.pvrtc2) : parse_colorA(w01, g.pvrtc2);
        C5 c11 = pass ? parse_colorB(w11, g.pvrtc2) : parse_colorA(w11, g.pvrtc2);
        int cr = c00.r * k00 + c10.r * k10 + c01.r * k01 + c11.r * k11;
        int cg = c00.g * k00 + c10.g * k10 + c01.g * k01 + c11.g * k11;
        int cb = c00.b * k00 + c10.b * k10 + c01.b * k01 + c11.b * k11;
        int ca = c00.a * k00 + c10.a * k10 + c01.a * k01 + c11.a * k11;
        Px out;
        if (bw == 4) {          /* 5.4 / 4.4 fixed point */
            out.r = cr / 2 + cr / 64;
            out.g = cg / 2 + cg / 64;
            out.b = cb / 2 + cb / 64;
            out.a = ca + ca / 16;
        } else {                /* 5.5 / 4.5 fixed point */
            out.r = cr / 4 + cr / 128;
            out.g = cg / 4 + cg / 128;
            out.b = cb / 4 + cb / 128;
            out.a = ca / 2 + ca / 32;
        }
        if (pass) B = out; else A = out;
    }
}

/* ---------------------------------------------------------- modulation --- */

const int kWeightStd[4] = { 0, 3, 5, 8 };
const int kWeightPT[4]  = { 0, 4, 4, 8 };
const int kRep[4]       = { 0, 3, 5, 8 };

/* 4bpp: modulation weight for texel (x, y); pt set on punch-through. */
int weight4(const Grid &g, int x, int y, bool &pt) {
    int X = x >> 2, Y = y >> 2, lx = x & 3, ly = y & 3;
    int v = (int)((g.modw(X, Y) >> (2 * (ly * 4 + lx))) & 3);
    pt = false;
    if (g.colw(X, Y) & 1) {          /* M = 1: punch-through weights */
        if (v == 2) pt = true;
        return kWeightPT[v];
    }
    return kWeightStd[v];
}

/* 2bpp: stored 2-bit value of a *stored-parity* texel at (x, y), honouring
 * the containing word's mode and the repurposed bits of texels (0,0) and
 * (4,2). Direct-mode words yield 0 or 3. */
int stored2(const Grid &g, int x, int y) {
    int X = x >> 3, Y = y >> 2, lx = x & 7, ly = y & 3;
    uint32_t mbits = g.modw(X, Y);
    if ((g.colw(X, Y) & 1) == 0)                 /* direct 1bpp word */
        return ((mbits >> (ly * 8 + lx)) & 1) ? 3 : 0;
    int slot = ly * 4 + (lx >> 1);
    if (slot == 0)                               /* bit0 repurposed as flag */
        return (mbits & 2) ? 3 : 0;
    if ((mbits & 1) && slot == 10)               /* bit20 repurposed as F */
        return (mbits & (1u << 21)) ? 3 : 0;
    return (int)((mbits >> (2 * slot)) & 3);
}

/* 2bpp: modulation weight (0..8) for texel (x, y). Neighbour lookups wrap
 * within the block-padded texel lattice. */
int weight2(const Grid &g, int x, int y) {
    int X = x >> 3, Y = y >> 2, lx = x & 7, ly = y & 3;
    uint32_t cw = g.colw(X, Y);
    if ((cw & 1) == 0) {                         /* direct: A or B */
        uint32_t mbits = g.modw(X, Y);
        return ((mbits >> (ly * 8 + lx)) & 1) ? 8 : 0;
    }
    if (((lx ^ ly) & 1) == 0)                    /* stored texel */
        return kRep[stored2(g, x, y)];
    uint32_t mbits = g.modw(X, Y);
    int pw = g.wx * 8, ph = g.wy * 4;
    int l = kRep[stored2(g, wrapi(x - 1, pw), y)];
    int r = kRep[stored2(g, wrapi(x + 1, pw), y)];
    int u = kRep[stored2(g, x, wrapi(y - 1, ph))];
    int d = kRep[stored2(g, x, wrapi(y + 1, ph))];
    if ((mbits & 1) == 0) return (l + r + u + d + 2) / 4;   /* H & V */
    if (mbits & (1u << 20)) return (u + d + 1) / 2;         /* V only */
    return (l + r + 1) / 2;                                 /* H only */
}

inline int blend8(int a, int b, int w) { return (a * (8 - w) + b * w) / 8; }

/* --------------------------------------------------------------------- */
/* PVRTC1                                                                 */
/* --------------------------------------------------------------------- */

int pvrtc1_check_dims(bool bpp2, uint32_t w, uint32_t h) {
    uint32_t bw = bpp2 ? 8u : 4u;
    if (!ispow2(w) || !ispow2(h) || w < bw || h < 4)
        return TEXC_ERR_BAD_DIMENSIONS;
    return TEXC_OK;
}

int pvrtc1_decode(bool bpp2, bool alpha_fmt, const uint8_t *src,
                  size_t src_size, uint32_t width, uint32_t height,
                  uint8_t *dst) {
    int rc = pvrtc1_check_dims(bpp2, width, height);
    if (rc != TEXC_OK) return rc;

    const int bw = bpp2 ? 8 : 4;
    const int wx = (int)width / bw, wy = (int)height / 4;
    const int sx = wx < 2 ? 2 : wx, sy = wy < 2 ? 2 : wy;
    if (src_size < (size_t)sx * sy * 8) return TEXC_ERR_BUFFER_TOO_SMALL;

    Grid g;
    g.wx = wx; g.wy = wy; g.bw = bw; g.wrap = true; g.pvrtc2 = false;
    g.col.resize((size_t)wx * wy);
    g.mod.resize((size_t)wx * wy);
    for (int Y = 0; Y < wy; Y++)
        for (int X = 0; X < wx; X++) {
            size_t off = (size_t)morton_offset((uint32_t)X, (uint32_t)Y,
                                               (uint32_t)sx, (uint32_t)sy) * 8;
            g.mod[(size_t)Y * wx + X] = load32le(src + off);
            g.col[(size_t)Y * wx + X] = load32le(src + off + 4);
        }

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = dst + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            bool pt = false;
            int w = bpp2 ? weight2(g, (int)x, (int)y)
                         : weight4(g, (int)x, (int)y, pt);
            Px A, B;
            upscale(g, (int)x, (int)y, A, B);
            int r = blend8(A.r, B.r, w);
            int gg = blend8(A.g, B.g, w);
            int b = blend8(A.b, B.b, w);
            int a = pt ? 0 : blend8(A.a, B.a, w);
            if (!alpha_fmt) a = 255;
            row[x * 4 + 0] = (uint8_t)r;
            row[x * 4 + 1] = (uint8_t)gg;
            row[x * 4 + 2] = (uint8_t)b;
            row[x * 4 + 3] = (uint8_t)a;
        }
    }
    return TEXC_OK;
}

/* ------------------------------------------------------------ encoding --- */

/* Best n-bit RGB code for an 8-bit target under the two-step replication
 * expansion (n -> 5 -> 8 bits). */
int quant_rgb(int v8, int bits) {
    int n = 1 << bits, best = 0, bestd = 1 << 30;
    for (int i = 0; i < n; i++) {
        int c5 = (bits == 5) ? i
               : (bits == 4) ? ((i << 1) | (i >> 3))
                             : ((i << 2) | (i >> 1));
        int c8 = (c5 << 3) | (c5 >> 2);
        int d = c8 - v8;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

/* Best 3-bit alpha code; expansion is ((a<<1)|lowbit) * 17. */
int quant_a3(int v8, int lowbit) {
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < 8; i++) {
        int d = ((i << 1) | lowbit) * 17 - v8;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

uint32_t make_colorword_pvrtc1(const int mn[4], const int mx[4],
                               bool alpha_fmt, bool mod_flag) {
    uint32_t cw = mod_flag ? 1u : 0u;
    if (!alpha_fmt || mn[3] >= 248) {            /* colour A opaque (554) */
        cw |= 0x8000u |
              ((uint32_t)quant_rgb(mn[0], 5) << 10) |
              ((uint32_t)quant_rgb(mn[1], 5) << 5) |
              ((uint32_t)quant_rgb(mn[2], 4) << 1);
    } else {                                     /* colour A 3443 */
        cw |= ((uint32_t)quant_a3(mn[3], 0) << 12) |
              ((uint32_t)quant_rgb(mn[0], 4) << 8) |
              ((uint32_t)quant_rgb(mn[1], 4) << 4) |
              ((uint32_t)quant_rgb(mn[2], 3) << 1);
    }
    if (!alpha_fmt || mx[3] >= 248) {            /* colour B opaque (555) */
        cw |= 0x80000000u |
              ((uint32_t)quant_rgb(mx[0], 5) << 26) |
              ((uint32_t)quant_rgb(mx[1], 5) << 21) |
              ((uint32_t)quant_rgb(mx[2], 5) << 16);
    } else {                                     /* colour B 3444 */
        cw |= ((uint32_t)quant_a3(mx[3], 0) << 28) |
              ((uint32_t)quant_rgb(mx[0], 4) << 24) |
              ((uint32_t)quant_rgb(mx[1], 4) << 20) |
              ((uint32_t)quant_rgb(mx[2], 4) << 16);
    }
    return cw;
}

/* Squared error of blending A/B with weight w against a target texel. */
inline long blend_err(const Px &A, const Px &B, int w, const int t[4]) {
    long e = 0, d;
    d = blend8(A.r, B.r, w) - t[0]; e += d * d;
    d = blend8(A.g, B.g, w) - t[1]; e += d * d;
    d = blend8(A.b, B.b, w) - t[2]; e += d * d;
    d = blend8(A.a, B.a, w) - t[3]; e += d * d;
    return e;
}

int pvrtc1_encode(bool bpp2, bool alpha_fmt, const uint8_t *src,
                  uint32_t width, uint32_t height, uint8_t *dst) {
    int rc = pvrtc1_check_dims(bpp2, width, height);
    if (rc != TEXC_OK) return rc;

    const int bw = bpp2 ? 8 : 4;
    const int wx = (int)width / bw, wy = (int)height / 4;
    const int sx = wx < 2 ? 2 : wx, sy = wy < 2 ? 2 : wy;

    Grid g;
    g.wx = wx; g.wy = wy; g.bw = bw; g.wrap = true; g.pvrtc2 = false;
    g.col.resize((size_t)wx * wy);
    g.mod.assign((size_t)wx * wy, 0);

    /* Pass 1: per-word low-frequency endpoints - component-wise min/max
     * over a window centred on the word's colour sample (word centre
     * +/- bw/2 horizontally, +/- 2 vertically), sampled with the same
     * toroidal wrap the decoder uses. Sampling around the centre (rather
     * than the word's own block) keeps interior endpoints tight while
     * letting words that straddle the wrap seam bracket both sides of the
     * image, which greatly reduces the border artefacts of non-tiling
     * content. */
    for (int Y = 0; Y < wy; Y++)
        for (int X = 0; X < wx; X++) {
            int mn[4] = { 255, 255, 255, 255 }, mx[4] = { 0, 0, 0, 0 };
            int cx = X * bw + bw / 2, cy = Y * 4 + 2;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -bw / 2; dx <= bw / 2; dx++) {
                    int x = wrapi(cx + dx, (int)width);
                    int y = wrapi(cy + dy, (int)height);
                    const uint8_t *p = src + ((size_t)y * width + x) * 4;
                    for (int c = 0; c < 4; c++) {
                        int v = (c == 3 && !alpha_fmt) ? 255 : p[c];
                        if (v < mn[c]) mn[c] = v;
                        if (v > mx[c]) mx[c] = v;
                    }
                }
            g.col[(size_t)Y * wx + X] =
                make_colorword_pvrtc1(mn, mx, alpha_fmt, bpp2);
        }

    /* Pass 2: per-texel modulation against the exact reconstruction. */
    for (int Y = 0; Y < wy; Y++)
        for (int X = 0; X < wx; X++) {
            uint32_t bits = 0;
            if (!bpp2) {
                for (int ly = 0; ly < 4; ly++)
                    for (int lx = 0; lx < 4; lx++) {
                        int x = X * 4 + lx, y = Y * 4 + ly;
                        Px A, B;
                        upscale(g, x, y, A, B);
                        const uint8_t *p = src + ((size_t)y * width + x) * 4;
                        int t[4] = { p[0], p[1], p[2],
                                     alpha_fmt ? p[3] : 255 };
                        int best = 0;
                        long bestE = blend_err(A, B, kWeightStd[0], t);
                        for (int v = 1; v < 4; v++) {
                            long e = blend_err(A, B, kWeightStd[v], t);
                            if (e < bestE) { bestE = e; best = v; }
                        }
                        bits |= (uint32_t)best << (2 * (ly * 4 + lx));
                    }
            } else {
                /* Checkerboard-interpolated submode H&V (bit0 = 0). Texel
                 * (0,0)'s value lives in bit1 only and encodes 0 or 8. */
                for (int slot = 0; slot < 16; slot++) {
                    int ly = slot >> 2, lx = 2 * (slot & 3) + (ly & 1);
                    int x = X * 8 + lx, y = Y * 4 + ly;
                    Px A, B;
                    upscale(g, x, y, A, B);
                    const uint8_t *p = src + ((size_t)y * width + x) * 4;
                    int t[4] = { p[0], p[1], p[2], alpha_fmt ? p[3] : 255 };
                    if (slot == 0) {
                        if (blend_err(A, B, 8, t) < blend_err(A, B, 0, t))
                            bits |= 2u;
                    } else {
                        int best = 0;
                        long bestE = blend_err(A, B, kRep[0], t);
                        for (int v = 1; v < 4; v++) {
                            long e = blend_err(A, B, kRep[v], t);
                            if (e < bestE) { bestE = e; best = v; }
                        }
                        bits |= (uint32_t)best << (2 * slot);
                    }
                }
            }
            g.mod[(size_t)Y * wx + X] = bits;
        }

    /* Write the Morton-ordered stored grid; padding words (when the image is
     * narrower/shorter than two words) replicate the logical words. */
    for (int SY = 0; SY < sy; SY++)
        for (int SX = 0; SX < sx; SX++) {
            int X = SX % wx, Y = SY % wy;
            size_t off = (size_t)morton_offset((uint32_t)SX, (uint32_t)SY,
                                               (uint32_t)sx, (uint32_t)sy) * 8;
            store32le(dst + off, g.mod[(size_t)Y * wx + X]);
            store32le(dst + off + 4, g.col[(size_t)Y * wx + X]);
        }
    return TEXC_OK;
}

/* --------------------------------------------------------------------- */
/* PVRTC2                                                                 */
/* --------------------------------------------------------------------- */

/* Local palette mode colour codes:
 * 0..7 = A/B of words P, Q, R, S; 8 = (5A_P+3B_P)/8; 9 = (3A_P+5B_P)/8.
 * Indexed [yr][xr][modulation value] per the Khronos spec table. */
const uint8_t kLocalPalette[4][4][4] = {
    { { 0, 8, 9, 1 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 } },
    { { 0, 1, 4, 5 }, { 0, 1, 2, 5 }, { 0, 1, 2, 3 }, { 6, 1, 2, 3 } },
    { { 0, 1, 4, 5 }, { 0, 1, 4, 5 }, { 0, 7, 4, 3 }, { 6, 7, 2, 3 } },
    { { 0, 1, 4, 5 }, { 0, 7, 4, 5 }, { 6, 7, 4, 5 }, { 6, 7, 4, 3 } },
};

Px local_palette_color(const Grid &g, int X0, int Y0, int X1, int Y1,
                       int code) {
    if (code >= 8) {
        Px a = c5_to_px(parse_colorA(g.colw(X0, Y0), true));
        Px b = c5_to_px(parse_colorB(g.colw(X0, Y0), true));
        int wa = (code == 8) ? 5 : 3, wb = 8 - wa;
        Px o;
        o.r = (wa * a.r + wb * b.r) / 8;
        o.g = (wa * a.g + wb * b.g) / 8;
        o.b = (wa * a.b + wb * b.b) / 8;
        o.a = (wa * a.a + wb * b.a) / 8;
        return o;
    }
    int X = (code & 2) ? X1 : X0;
    int Y = (code & 4) ? Y1 : Y0;
    uint32_t cw = g.colw(X, Y);
    return (code & 1) ? c5_to_px(parse_colorB(cw, true))
                      : c5_to_px(parse_colorA(cw, true));
}

int pvrtc2_decode(bool bpp2, const uint8_t *src, size_t src_size,
                  uint32_t width, uint32_t height, uint8_t *dst) {
    const int bw = bpp2 ? 8 : 4;
    const int wx = ((int)width + bw - 1) / bw, wy = ((int)height + 3) / 4;
    if (src_size < (size_t)wx * wy * 8) return TEXC_ERR_BUFFER_TOO_SMALL;

    Grid g;
    g.wx = wx; g.wy = wy; g.bw = bw; g.wrap = false; g.pvrtc2 = true;
    g.col.resize((size_t)wx * wy);
    g.mod.resize((size_t)wx * wy);
    for (size_t i = 0; i < (size_t)wx * wy; i++) {
        g.mod[i] = load32le(src + i * 8);
        g.col[i] = load32le(src + i * 8 + 4);
    }

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = dst + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            int X = (int)x / bw, Y = (int)y / 4;
            uint32_t cw = g.colw(X, Y);
            bool M = (cw & 1) != 0;
            int XL = floordiv((int)x - bw / 2, bw), YL = floordiv((int)y - 2, 4);
            int X0 = clampidx(XL, wx), X1 = clampidx(XL + 1, wx);
            int Y0 = clampidx(YL, wy), Y1 = clampidx(YL + 1, wy);
            bool H = ((g.colw(X0, Y0) >> 15) & 1) != 0;

            int r, gg, b, a;
            if (!bpp2) {
                int lx = (int)x & 3, ly = (int)y & 3;
                int v = (int)((g.modw(X, Y) >> (2 * (ly * 4 + lx))) & 3);
                if (!H) {
                    if (M && v == 2) {
                        /* punch-through: transparent black */
                        r = gg = b = a = 0;
                    } else {
                        Px A, B;
                        upscale(g, (int)x, (int)y, A, B);
                        int w = M ? kWeightPT[v] : kWeightStd[v];
                        r = blend8(A.r, B.r, w);
                        gg = blend8(A.g, B.g, w);
                        b = blend8(A.b, B.b, w);
                        a = blend8(A.a, B.a, w);
                    }
                } else if (!M) {
                    /* non-interpolated: this word's colours, replicated */
                    Px A = c5_to_px(parse_colorA(cw, true));
                    Px B = c5_to_px(parse_colorB(cw, true));
                    int w = kWeightStd[v];
                    r = blend8(A.r, B.r, w);
                    gg = blend8(A.g, B.g, w);
                    b = blend8(A.b, B.b, w);
                    a = blend8(A.a, B.a, w);
                } else {
                    /* local palette */
                    int xr = ((int)x - 2) - 4 * XL, yr = ((int)y - 2) - 4 * YL;
                    Px c = local_palette_color(
                        g, X0, Y0, X1, Y1, kLocalPalette[yr][xr][v]);
                    r = c.r; gg = c.g; b = c.b; a = c.a;
                }
            } else {
                int w = weight2(g, (int)x, (int)y);
                Px A, B;
                if (!H) {
                    upscale(g, (int)x, (int)y, A, B);
                } else {
                    A = c5_to_px(parse_colorA(cw, true));
                    B = c5_to_px(parse_colorB(cw, true));
                }
                r = blend8(A.r, B.r, w);
                gg = blend8(A.g, B.g, w);
                b = blend8(A.b, B.b, w);
                a = blend8(A.a, B.a, w);
            }
            row[x * 4 + 0] = (uint8_t)r;
            row[x * 4 + 1] = (uint8_t)gg;
            row[x * 4 + 2] = (uint8_t)b;
            row[x * 4 + 3] = (uint8_t)a;
        }
    }
    return TEXC_OK;
}

uint32_t make_colorword_pvrtc2(const int mn[4], const int mx[4],
                               bool hard, bool mod_flag) {
    uint32_t cw = (hard ? 0x8000u : 0u) | (mod_flag ? 1u : 0u);
    bool opaque = mn[3] >= 248;                 /* single opacity flag */
    if (opaque) {
        cw |= 0x80000000u |
              ((uint32_t)quant_rgb(mn[0], 5) << 10) |
              ((uint32_t)quant_rgb(mn[1], 5) << 5) |
              ((uint32_t)quant_rgb(mn[2], 4) << 1) |
              ((uint32_t)quant_rgb(mx[0], 5) << 26) |
              ((uint32_t)quant_rgb(mx[1], 5) << 21) |
              ((uint32_t)quant_rgb(mx[2], 5) << 16);
    } else {
        cw |= ((uint32_t)quant_a3(mn[3], 0) << 12) |
              ((uint32_t)quant_rgb(mn[0], 4) << 8) |
              ((uint32_t)quant_rgb(mn[1], 4) << 4) |
              ((uint32_t)quant_rgb(mn[2], 3) << 1) |
              ((uint32_t)quant_a3(mx[3], 1) << 28) |
              ((uint32_t)quant_rgb(mx[0], 4) << 24) |
              ((uint32_t)quant_rgb(mx[1], 4) << 20) |
              ((uint32_t)quant_rgb(mx[2], 4) << 16);
    }
    return cw;
}

/* PVRTC2 encoder: hard-transition + non-interpolated mode on every word so
 * each block is self-contained (local A/B palette + per-texel modulation). */
int pvrtc2_encode(bool bpp2, const uint8_t *src,
                  uint32_t width, uint32_t height, uint8_t *dst) {
    const int bw = bpp2 ? 8 : 4;
    const int wx = ((int)width + bw - 1) / bw, wy = ((int)height + 3) / 4;

    std::vector<uint8_t> blk((size_t)bw * 4 * 4);
    for (int Y = 0; Y < wy; Y++)
        for (int X = 0; X < wx; X++) {
            read_block_rgba8(src, width, height, (uint32_t)X, (uint32_t)Y,
                             (uint32_t)bw, 4, blk.data());
            int mn[4] = { 255, 255, 255, 255 }, mx[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < bw * 4; i++)
                for (int c = 0; c < 4; c++) {
                    int v = blk[(size_t)i * 4 + c];
                    if (v < mn[c]) mn[c] = v;
                    if (v > mx[c]) mx[c] = v;
                }
            uint32_t cw = make_colorword_pvrtc2(mn, mx, true, bpp2);
            Px A = c5_to_px(parse_colorA(cw, true));
            Px B = c5_to_px(parse_colorB(cw, true));

            uint32_t bits = 0;
            if (!bpp2) {
                for (int i = 0; i < 16; i++) {
                    int t[4] = { blk[(size_t)i * 4 + 0], blk[(size_t)i * 4 + 1],
                                 blk[(size_t)i * 4 + 2], blk[(size_t)i * 4 + 3] };
                    int best = 0;
                    long bestE = blend_err(A, B, kWeightStd[0], t);
                    for (int v = 1; v < 4; v++) {
                        long e = blend_err(A, B, kWeightStd[v], t);
                        if (e < bestE) { bestE = e; best = v; }
                    }
                    bits |= (uint32_t)best << (2 * i);
                }
            } else {
                /* M = 1: checkerboard-interpolated modulation, H&V submode */
                for (int slot = 0; slot < 16; slot++) {
                    int ly = slot >> 2, lx = 2 * (slot & 3) + (ly & 1);
                    const uint8_t *p = &blk[((size_t)ly * 8 + lx) * 4];
                    int t[4] = { p[0], p[1], p[2], p[3] };
                    if (slot == 0) {
                        if (blend_err(A, B, 8, t) < blend_err(A, B, 0, t))
                            bits |= 2u;
                    } else {
                        int best = 0;
                        long bestE = blend_err(A, B, kRep[0], t);
                        for (int v = 1; v < 4; v++) {
                            long e = blend_err(A, B, kRep[v], t);
                            if (e < bestE) { bestE = e; best = v; }
                        }
                        bits |= (uint32_t)best << (2 * slot);
                    }
                }
            }
            size_t off = ((size_t)Y * wx + X) * 8;
            store32le(dst + off, bits);
            store32le(dst + off + 4, cw);
        }
    return TEXC_OK;
}

} /* anonymous namespace */

/* ----------------------------------------------------------- dispatch --- */

int pvrtc_decode(texc_format fmt, const uint8_t *src, size_t src_size,
                 uint32_t width, uint32_t height, uint8_t *dst) {
    switch (fmt) {
        case TEXC_FORMAT_PVRTC1_2BPP_RGB:
            return pvrtc1_decode(true, false, src, src_size, width, height, dst);
        case TEXC_FORMAT_PVRTC1_2BPP_RGBA:
            return pvrtc1_decode(true, true, src, src_size, width, height, dst);
        case TEXC_FORMAT_PVRTC1_4BPP_RGB:
            return pvrtc1_decode(false, false, src, src_size, width, height, dst);
        case TEXC_FORMAT_PVRTC1_4BPP_RGBA:
            return pvrtc1_decode(false, true, src, src_size, width, height, dst);
        case TEXC_FORMAT_PVRTC2_2BPP:
            return pvrtc2_decode(true, src, src_size, width, height, dst);
        case TEXC_FORMAT_PVRTC2_4BPP:
            return pvrtc2_decode(false, src, src_size, width, height, dst);
        default:
            return TEXC_ERR_UNSUPPORTED;
    }
}

int pvrtc_encode(texc_format fmt, const uint8_t *src,
                 uint32_t width, uint32_t height, uint8_t *dst,
                 const texc_encode_options *opts) {
    (void)opts;                     /* no PVRTC-specific options yet */
    switch (fmt) {
        case TEXC_FORMAT_PVRTC1_2BPP_RGB:
            return pvrtc1_encode(true, false, src, width, height, dst);
        case TEXC_FORMAT_PVRTC1_2BPP_RGBA:
            return pvrtc1_encode(true, true, src, width, height, dst);
        case TEXC_FORMAT_PVRTC1_4BPP_RGB:
            return pvrtc1_encode(false, false, src, width, height, dst);
        case TEXC_FORMAT_PVRTC1_4BPP_RGBA:
            return pvrtc1_encode(false, true, src, width, height, dst);
        case TEXC_FORMAT_PVRTC2_2BPP:
            return pvrtc2_encode(true, src, width, height, dst);
        case TEXC_FORMAT_PVRTC2_4BPP:
            return pvrtc2_encode(false, src, width, height, dst);
        default:
            return TEXC_ERR_UNSUPPORTED;
    }
}

} /* namespace texc */

/* ===================================================================== */
/* Self-test                                                             */
/* ===================================================================== */
#ifdef TEXC_SELFTEST

#include <cmath>
#include <cstdio>

using namespace texc;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            printf("FAIL(%d): ", __LINE__);                                 \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static size_t pvrtc_size(texc_format fmt, uint32_t w, uint32_t h) {
    bool bpp2 = fmt == TEXC_FORMAT_PVRTC1_2BPP_RGB ||
                fmt == TEXC_FORMAT_PVRTC1_2BPP_RGBA ||
                fmt == TEXC_FORMAT_PVRTC2_2BPP;
    bool pv1 = fmt >= TEXC_FORMAT_PVRTC1_2BPP_RGB &&
               fmt <= TEXC_FORMAT_PVRTC1_4BPP_RGBA;
    uint32_t bw = bpp2 ? 8 : 4;
    uint32_t bx = (w + bw - 1) / bw, by = (h + 3) / 4;
    if (pv1) {
        if (bx < 2) bx = 2;
        if (by < 2) by = 2;
    }
    return (size_t)bx * by * 8;
}

static size_t atc_size(texc_format fmt, uint32_t w, uint32_t h) {
    size_t bb = (fmt == TEXC_FORMAT_ATC_RGB) ? 8 : 16;
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * bb;
}

static double psnr(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
                   int channels) {
    double mse = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < channels; c++) {
            double d = (double)a[i + c] - (double)b[i + c];
            mse += d * d;
            n++;
        }
    mse /= (double)n;
    if (mse <= 0.0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

static std::vector<uint8_t> gradient(uint32_t w, uint32_t h, bool alpha_grad) {
    std::vector<uint8_t> img((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = &img[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
            p[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            p[2] = (uint8_t)((x + y) * 255 / (w + h > 2 ? w + h - 2 : 1));
            p[3] = alpha_grad
                       ? (uint8_t)(64 + (x + y) * 191 / (w + h > 2 ? w + h - 2 : 1))
                       : 255;
        }
    return img;
}

/* Fixed-slope smooth gradient (independent of image size). */
static std::vector<uint8_t> soft_gradient(uint32_t w, uint32_t h,
                                          bool alpha_grad) {
    std::vector<uint8_t> img((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = &img[((size_t)y * w + x) * 4];
            p[0] = clamp_u8((int)(x * 6));
            p[1] = clamp_u8((int)(y * 6));
            p[2] = clamp_u8(32 + (int)(x * 3 + y * 3));
            p[3] = alpha_grad ? clamp_u8(60 + (int)(x * 4 + y * 2)) : 255;
        }
    return img;
}

static std::vector<uint8_t> uniform(uint32_t w, uint32_t h,
                                    int r, int g, int b, int a) {
    std::vector<uint8_t> img((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        img[i * 4 + 0] = (uint8_t)r;
        img[i * 4 + 1] = (uint8_t)g;
        img[i * 4 + 2] = (uint8_t)b;
        img[i * 4 + 3] = (uint8_t)a;
    }
    return img;
}

/* encode+decode roundtrip PSNR for a PVRTC format */
static double roundtrip_pvrtc(texc_format fmt, uint32_t w, uint32_t h,
                              const std::vector<uint8_t> &img, int channels,
                              std::vector<uint8_t> *out = nullptr) {
    std::vector<uint8_t> enc(pvrtc_size(fmt, w, h), 0);
    std::vector<uint8_t> dec((size_t)w * h * 4, 0);
    int r1 = pvrtc_encode(fmt, img.data(), w, h, enc.data(), nullptr);
    int r2 = pvrtc_decode(fmt, enc.data(), enc.size(), w, h, dec.data());
    CHECK(r1 == TEXC_OK, "pvrtc_encode fmt=%d rc=%d", (int)fmt, r1);
    CHECK(r2 == TEXC_OK, "pvrtc_decode fmt=%d rc=%d", (int)fmt, r2);
    if (out) *out = dec;
    return psnr(img, dec, channels);
}

static double roundtrip_atc(texc_format fmt, uint32_t w, uint32_t h,
                            const std::vector<uint8_t> &img, int channels) {
    std::vector<uint8_t> enc(atc_size(fmt, w, h), 0);
    std::vector<uint8_t> dec((size_t)w * h * 4, 0);
    int r1 = atc_encode(fmt, img.data(), w, h, enc.data(), nullptr);
    int r2 = atc_decode(fmt, enc.data(), enc.size(), w, h, dec.data());
    CHECK(r1 == TEXC_OK, "atc_encode fmt=%d rc=%d", (int)fmt, r1);
    CHECK(r2 == TEXC_OK, "atc_decode fmt=%d rc=%d", (int)fmt, r2);
    return psnr(img, dec, channels);
}

static int max_channel_err(const std::vector<uint8_t> &a,
                           const std::vector<uint8_t> &b, int channels) {
    int m = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < channels; c++) {
            int d = (int)a[i + c] - (int)b[i + c];
            if (d < 0) d = -d;
            if (d > m) m = d;
        }
    return m;
}

/* ------------------------------------------------------------- PVRTC1 --- */

static void test_pvrtc1_roundtrip() {
    const struct { texc_format fmt; const char *name; int ch; bool ag; } cases[] = {
        { TEXC_FORMAT_PVRTC1_4BPP_RGB,  "PVRTC1_4BPP_RGB",  3, false },
        { TEXC_FORMAT_PVRTC1_4BPP_RGBA, "PVRTC1_4BPP_RGBA", 4, true  },
        { TEXC_FORMAT_PVRTC1_2BPP_RGB,  "PVRTC1_2BPP_RGB",  3, false },
        { TEXC_FORMAT_PVRTC1_2BPP_RGBA, "PVRTC1_2BPP_RGBA", 4, true  },
    };
    for (const auto &c : cases) {
        std::vector<uint8_t> img = gradient(64, 64, c.ag);
        double db = roundtrip_pvrtc(c.fmt, 64, 64, img, c.ch);
        printf("  %-18s 64x64 gradient roundtrip: %.2f dB\n", c.name, db);
        CHECK(db >= 24.0, "%s gradient PSNR %.2f < 24", c.name, db);
    }
}

static void test_pvrtc1_uniform() {
    /* (82,165,82) is exactly representable in both 554 and 555. */
    for (int bpp2 = 0; bpp2 < 2; bpp2++) {
        texc_format fmt = bpp2 ? TEXC_FORMAT_PVRTC1_2BPP_RGB
                               : TEXC_FORMAT_PVRTC1_4BPP_RGB;
        std::vector<uint8_t> img = uniform(64, 64, 82, 165, 82, 255), dec;
        roundtrip_pvrtc(fmt, 64, 64, img, 3, &dec);
        CHECK(max_channel_err(img, dec, 3) == 0,
              "PVRTC1 %dbpp uniform (82,165,82) not exact (err=%d)",
              bpp2 ? 2 : 4, max_channel_err(img, dec, 3));
    }
    /* (99,173,222,136) is exactly representable in translucent 3443/3444. */
    {
        std::vector<uint8_t> img = uniform(32, 32, 99, 173, 222, 136), dec;
        roundtrip_pvrtc(TEXC_FORMAT_PVRTC1_4BPP_RGBA, 32, 32, img, 4, &dec);
        CHECK(max_channel_err(img, dec, 4) == 0,
              "PVRTC1 4bpp translucent uniform not exact (err=%d)",
              max_channel_err(img, dec, 4));
    }
    /* Arbitrary uniform colour: near-exact. */
    {
        std::vector<uint8_t> img = uniform(32, 32, 37, 180, 66, 255), dec;
        roundtrip_pvrtc(TEXC_FORMAT_PVRTC1_4BPP_RGB, 32, 32, img, 3, &dec);
        int err = max_channel_err(img, dec, 3);
        CHECK(err <= 8, "PVRTC1 uniform (37,180,66) err %d > 8", err);
    }
}

static void test_pvrtc1_bad_dims() {
    std::vector<uint8_t> buf(64 * 64, 0), out(64 * 64 * 4, 0);
    CHECK(pvrtc_decode(TEXC_FORMAT_PVRTC1_4BPP_RGB, buf.data(), buf.size(),
                       48, 32, out.data()) == TEXC_ERR_BAD_DIMENSIONS,
          "PVRTC1 decode 48x32 should be BAD_DIMENSIONS");
    CHECK(pvrtc_encode(TEXC_FORMAT_PVRTC1_4BPP_RGB, out.data(),
                       48, 32, buf.data(), nullptr) == TEXC_ERR_BAD_DIMENSIONS,
          "PVRTC1 encode 48x32 should be BAD_DIMENSIONS");
    CHECK(pvrtc_decode(TEXC_FORMAT_PVRTC1_2BPP_RGB, buf.data(), buf.size(),
                       4, 4, out.data()) == TEXC_ERR_BAD_DIMENSIONS,
          "PVRTC1 2bpp decode 4x4 (< one block) should be BAD_DIMENSIONS");
    CHECK(pvrtc_decode(TEXC_FORMAT_PVRTC1_4BPP_RGB, buf.data(), buf.size(),
                       64, 33, out.data()) == TEXC_ERR_BAD_DIMENSIONS,
          "PVRTC1 decode 64x33 should be BAD_DIMENSIONS");
}

/* Known-answer: hand-built uniform words must decode to the exact colour. */
static void test_pvrtc1_known_answer() {
    /* colour: r5=10 g5=20, A blue4=5 / B blue5=10 -> RGB (82,165,82) */
    uint32_t colw4 = 0x80000000u | (10u << 26) | (20u << 21) | (10u << 16) |
                     0x8000u | (10u << 10) | (20u << 5) | (5u << 1);
    struct { uint32_t w, h; bool bpp2; uint32_t modbits; } cases[] = {
        { 64, 64, false, 0xAAAAAAAAu },  /* 4bpp, weight 5 everywhere */
        { 8,  8,  false, 0x00000000u },  /* 4bpp minimum-ish image    */
        { 4,  4,  false, 0xFFFFFFFFu },  /* single logical word       */
        { 64, 64, true,  0xF0F0F0F0u },  /* 2bpp direct modulation    */
        { 16, 8,  true,  0x12345678u },
    };
    for (const auto &c : cases) {
        texc_format fmt = c.bpp2 ? TEXC_FORMAT_PVRTC1_2BPP_RGBA
                                 : TEXC_FORMAT_PVRTC1_4BPP_RGBA;
        size_t sz = pvrtc_size(fmt, c.w, c.h);
        std::vector<uint8_t> enc(sz);
        for (size_t off = 0; off < sz; off += 8) {
            uint32_t cw = colw4;         /* mode flag bit0 = 0 in all cases */
            enc[off + 0] = (uint8_t)(c.modbits & 0xFF);
            enc[off + 1] = (uint8_t)((c.modbits >> 8) & 0xFF);
            enc[off + 2] = (uint8_t)((c.modbits >> 16) & 0xFF);
            enc[off + 3] = (uint8_t)((c.modbits >> 24) & 0xFF);
            enc[off + 4] = (uint8_t)(cw & 0xFF);
            enc[off + 5] = (uint8_t)((cw >> 8) & 0xFF);
            enc[off + 6] = (uint8_t)((cw >> 16) & 0xFF);
            enc[off + 7] = (uint8_t)((cw >> 24) & 0xFF);
        }
        std::vector<uint8_t> dec((size_t)c.w * c.h * 4, 0);
        int rc = pvrtc_decode(fmt, enc.data(), enc.size(), c.w, c.h, dec.data());
        CHECK(rc == TEXC_OK, "PVRTC1 KAT decode rc=%d", rc);
        bool ok = true;
        for (size_t i = 0; i < dec.size(); i += 4)
            if (dec[i] != 82 || dec[i + 1] != 165 || dec[i + 2] != 82 ||
                dec[i + 3] != 255) { ok = false; break; }
        CHECK(ok, "PVRTC1 KAT %ux%u (%dbpp) not uniform (82,165,82,255)",
              c.w, c.h, c.bpp2 ? 2 : 4);
    }
}

/* ------------------------------------------------------------- PVRTC2 --- */

static void test_pvrtc2_roundtrip() {
    const struct { texc_format fmt; const char *name; } cases[] = {
        { TEXC_FORMAT_PVRTC2_4BPP, "PVRTC2_4BPP" },
        { TEXC_FORMAT_PVRTC2_2BPP, "PVRTC2_2BPP" },
    };
    const struct { uint32_t w, h; } sizes[] = { { 37, 23 }, { 64, 64 } };
    for (const auto &c : cases)
        for (const auto &s : sizes)
            for (int ag = 0; ag < 2; ag++) {
                std::vector<uint8_t> img = gradient(s.w, s.h, ag != 0);
                double db = roundtrip_pvrtc(c.fmt, s.w, s.h, img, 4);
                printf("  %-12s %ux%u %s gradient roundtrip: %.2f dB\n",
                       c.name, s.w, s.h, ag ? "RGBA" : "RGB ", db);
                CHECK(db >= 28.0, "%s %ux%u PSNR %.2f < 28",
                      c.name, s.w, s.h, db);
            }
}

/* --------------------------------------------------------------- ATC ---- */

static void test_atc_roundtrip() {
    const struct { texc_format fmt; const char *name; bool ag; } cases[] = {
        { TEXC_FORMAT_ATC_RGB,               "ATC_RGB",               false },
        { TEXC_FORMAT_ATC_RGBA_EXPLICIT,     "ATC_RGBA_EXPLICIT",     true  },
        { TEXC_FORMAT_ATC_RGBA_INTERPOLATED, "ATC_RGBA_INTERPOLATED", true  },
    };
    const struct { uint32_t w, h; } sizes[] = { { 16, 16 }, { 37, 23 } };
    for (const auto &c : cases)
        for (const auto &s : sizes) {
            std::vector<uint8_t> img = soft_gradient(s.w, s.h, c.ag);
            int ch = (c.fmt == TEXC_FORMAT_ATC_RGB) ? 3 : 4;
            double db = roundtrip_atc(c.fmt, s.w, s.h, img, ch);
            printf("  %-22s %ux%u gradient roundtrip: %.2f dB\n",
                   c.name, s.w, s.h, db);
            CHECK(db >= 30.0, "%s %ux%u PSNR %.2f < 30", c.name, s.w, s.h, db);
        }
}

static void test_atc_known_answer() {
    /* c0 == c1 == white: every palette entry is white -> exact regardless
     * of the selector bits. */
    {
        uint8_t blk[8];
        uint16_t c0 = (uint16_t)((31 << 10) | (31 << 5) | 31); /* mode 0 */
        uint16_t c1 = 0xFFFF;                                  /* 31,63,31 */
        blk[0] = (uint8_t)(c0 & 0xFF); blk[1] = (uint8_t)(c0 >> 8);
        blk[2] = (uint8_t)(c1 & 0xFF); blk[3] = (uint8_t)(c1 >> 8);
        blk[4] = 0xE4; blk[5] = 0x1B; blk[6] = 0xE4; blk[7] = 0x1B;
        std::vector<uint8_t> dec(4 * 4 * 4, 0);
        int rc = atc_decode(TEXC_FORMAT_ATC_RGB, blk, 8, 4, 4, dec.data());
        CHECK(rc == TEXC_OK, "ATC KAT decode rc=%d", rc);
        bool ok = true;
        for (size_t i = 0; i < dec.size(); i++)
            if (dec[i] != 255) { ok = false; break; }
        CHECK(ok, "ATC c0=c1 white block should decode to solid white");
    }
    /* c0 == c1 encoding (82,165,82) with all selectors 0 -> p0 == c0. */
    {
        uint8_t blk[8] = { 0 };
        uint16_t c0 = (uint16_t)((10 << 10) | (20 << 5) | 10);
        uint16_t c1 = (uint16_t)((10 << 11) | (41 << 5) | 10);
        blk[0] = (uint8_t)(c0 & 0xFF); blk[1] = (uint8_t)(c0 >> 8);
        blk[2] = (uint8_t)(c1 & 0xFF); blk[3] = (uint8_t)(c1 >> 8);
        std::vector<uint8_t> dec(4 * 4 * 4, 0);
        atc_decode(TEXC_FORMAT_ATC_RGB, blk, 8, 4, 4, dec.data());
        bool ok = true;
        for (size_t i = 0; i < dec.size(); i += 4)
            if (dec[i] != 82 || dec[i + 1] != 165 || dec[i + 2] != 82 ||
                dec[i + 3] != 255) { ok = false; break; }
        CHECK(ok, "ATC solid (82,165,82) block should decode exactly");
    }
    /* Encoder roundtrip of a solid representable colour must be exact. */
    {
        std::vector<uint8_t> img = uniform(16, 16, 82, 165, 82, 255);
        std::vector<uint8_t> enc(atc_size(TEXC_FORMAT_ATC_RGB, 16, 16));
        std::vector<uint8_t> dec(16 * 16 * 4, 0);
        atc_encode(TEXC_FORMAT_ATC_RGB, img.data(), 16, 16, enc.data(), nullptr);
        atc_decode(TEXC_FORMAT_ATC_RGB, enc.data(), enc.size(), 16, 16,
                   dec.data());
        CHECK(max_channel_err(img, dec, 3) == 0,
              "ATC solid roundtrip not exact (err=%d)",
              max_channel_err(img, dec, 3));
    }
}

int main() {
    printf("PVRTC1 roundtrip:\n");
    test_pvrtc1_roundtrip();
    printf("PVRTC1 uniform / dims / known-answer:\n");
    test_pvrtc1_uniform();
    test_pvrtc1_bad_dims();
    test_pvrtc1_known_answer();
    printf("PVRTC2 roundtrip:\n");
    test_pvrtc2_roundtrip();
    printf("ATC roundtrip:\n");
    test_atc_roundtrip();
    test_atc_known_answer();

    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#endif /* TEXC_SELFTEST */
