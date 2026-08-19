/*
 * atc.cpp - ATC (AMD_compressed_ATC_texture / Adreno) codec module
 * (see codec_common.h).
 *
 * Colour block (8 bytes, little-endian): uint16 c0, uint16 c1, uint32 of
 * 2-bit selectors (texel i = y*4+x at bits [2i+1:2i]).
 *   c0 is X555 (bit15 = mode flag, red 14..10, green 9..5, blue 4..0),
 *   c1 is 565  (red 15..11, green 10..5, blue 4..0).
 * Mode 0 (bit15 clear): palette { c0, (5*c0+3*c1)/8, (3*c0+5*c1)/8, c1 }.
 * Mode 1 (bit15 set):   palette { black, (c0-c1)/4, c0, c1 } where the
 *   subtraction wraps at 16 bits and the quotient is truncated to 8 bits.
 * This matches the texture2ddecoder reference (src/atc.rs) exactly.
 *
 * ATC_RGBA_EXPLICIT     = 8-byte BC2-style 4-bit alpha block + colour block.
 * ATC_RGBA_INTERPOLATED = 8-byte BC3-style interpolated alpha + colour block.
 *
 * Encoder: mode-0 colour with component-wise min/max endpoints and
 * nearest-palette-entry selectors; BC2/BC3-style alpha encoding.
 */

#include "codec_common.h"

namespace texc {
namespace {

inline uint16_t load16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t load32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void store16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
inline void store32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ------------------------------------------------------- colour decode --- */

/* Decode one 8-byte ATC colour block into a 4x4 RGBA8 block (alpha = 255). */
void decode_atc_color_block(const uint8_t *p, uint8_t *block) {
    uint16_t c0 = load16le(p);
    uint16_t c1 = load16le(p + 2);
    uint32_t sel = load32le(p + 4);

    uint8_t pal[4][3]; /* [entry][r,g,b] */

    uint8_t r0 = expand5((c0 >> 10) & 0x1F);
    uint8_t g0 = expand5((c0 >> 5) & 0x1F);
    uint8_t b0 = expand5(c0 & 0x1F);
    uint8_t r1 = expand5((c1 >> 11) & 0x1F);
    uint8_t g1 = expand6((c1 >> 5) & 0x3F);
    uint8_t b1 = expand5(c1 & 0x1F);

    if ((c0 & 0x8000) == 0) {
        /* mode 0: c0, 5/8-3/8 blends, c1 */
        pal[0][0] = r0;  pal[0][1] = g0;  pal[0][2] = b0;
        pal[3][0] = r1;  pal[3][1] = g1;  pal[3][2] = b1;
        for (int i = 0; i < 3; i++) {
            pal[1][i] = (uint8_t)((5u * pal[0][i] + 3u * pal[3][i]) / 8u);
            pal[2][i] = (uint8_t)((5u * pal[3][i] + 3u * pal[0][i]) / 8u);
        }
    } else {
        /* mode 1: black, (c0-c1)/4 (16-bit wrapping, 8-bit truncation),
         * c0, c1 -- matches the texture2ddecoder reference. */
        pal[0][0] = pal[0][1] = pal[0][2] = 0;
        pal[2][0] = r0;  pal[2][1] = g0;  pal[2][2] = b0;
        pal[3][0] = r1;  pal[3][1] = g1;  pal[3][2] = b1;
        for (int i = 0; i < 3; i++) {
            uint16_t d = (uint16_t)((uint16_t)pal[2][i] - (uint16_t)pal[3][i]);
            pal[1][i] = (uint8_t)(d / 4u);
        }
    }

    for (int i = 0; i < 16; i++) {
        uint32_t v = (sel >> (2 * i)) & 3u;
        block[i * 4 + 0] = pal[v][0];
        block[i * 4 + 1] = pal[v][1];
        block[i * 4 + 2] = pal[v][2];
        block[i * 4 + 3] = 255;
    }
}

/* BC2-style explicit alpha (8 bytes, 4 bits per texel, LSB-first). */
void decode_explicit_alpha(const uint8_t *p, uint8_t *block) {
    for (int i = 0; i < 16; i++) {
        uint32_t nib = (p[i >> 1] >> ((i & 1) * 4)) & 0xF;
        block[i * 4 + 3] = expand4(nib);
    }
}

/* BC3/BC4-style interpolated alpha (8 bytes). */
void decode_interpolated_alpha(const uint8_t *p, uint8_t *block) {
    int a0 = p[0], a1 = p[1];
    int pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; i++)
            pal[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    } else {
        for (int i = 2; i < 6; i++)
            pal[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)p[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++)
        block[i * 4 + 3] = (uint8_t)pal[(bits >> (3 * i)) & 7];
}

/* ------------------------------------------------------- colour encode --- */

inline uint32_t quant5(int v) { return (uint32_t)((v * 31 + 127) / 255); }
inline uint32_t quant6(int v) { return (uint32_t)((v * 63 + 127) / 255); }

/* Encode a 4x4 RGBA8 block into an 8-byte ATC colour block (mode 0). */
void encode_atc_color_block(const uint8_t *block, uint8_t *p) {
    int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) {
            int v = block[i * 4 + c];
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
        }

    uint16_t c0 = (uint16_t)((quant5(mn[0]) << 10) | (quant5(mn[1]) << 5) |
                             quant5(mn[2]));                /* bit15 = 0 */
    uint16_t c1 = (uint16_t)((quant5(mx[0]) << 11) | (quant6(mx[1]) << 5) |
                             quant5(mx[2]));

    /* Build the palette exactly as the decoder does. */
    int pal[4][3];
    pal[0][0] = expand5((c0 >> 10) & 0x1F);
    pal[0][1] = expand5((c0 >> 5) & 0x1F);
    pal[0][2] = expand5(c0 & 0x1F);
    pal[3][0] = expand5((c1 >> 11) & 0x1F);
    pal[3][1] = expand6((c1 >> 5) & 0x3F);
    pal[3][2] = expand5(c1 & 0x1F);
    for (int i = 0; i < 3; i++) {
        pal[1][i] = (5 * pal[0][i] + 3 * pal[3][i]) / 8;
        pal[2][i] = (5 * pal[3][i] + 3 * pal[0][i]) / 8;
    }

    uint32_t sel = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        long bestErr = 0x7FFFFFFFL;
        for (int v = 0; v < 4; v++) {
            long e = 0;
            for (int c = 0; c < 3; c++) {
                long d = (long)block[i * 4 + c] - pal[v][c];
                e += d * d;
            }
            if (e < bestErr) { bestErr = e; best = v; }
        }
        sel |= (uint32_t)best << (2 * i);
    }

    store16le(p, c0);
    store16le(p + 2, c1);
    store32le(p + 4, sel);
}

void encode_explicit_alpha(const uint8_t *block, uint8_t *p) {
    for (int i = 0; i < 8; i++) {
        uint32_t lo = (uint32_t)(block[(2 * i) * 4 + 3] * 15 + 127) / 255;
        uint32_t hi = (uint32_t)(block[(2 * i + 1) * 4 + 3] * 15 + 127) / 255;
        p[i] = (uint8_t)(lo | (hi << 4));
    }
}

void encode_interpolated_alpha(const uint8_t *block, uint8_t *p) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) {
        int a = block[i * 4 + 3];
        if (a < mn) mn = a;
        if (a > mx) mx = a;
    }
    int a0 = mx, a1 = mn; /* a0 >= a1 -> 8-value ramp (a0 > a1) or flat */
    int pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; i++)
            pal[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    } else {
        for (int i = 2; i < 6; i++)
            pal[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int a = block[i * 4 + 3];
        int best = 0, bestErr = 0x7FFFFFFF;
        for (int v = 0; v < 8; v++) {
            int d = a - pal[v];
            if (d < 0) d = -d;
            if (d < bestErr) { bestErr = d; best = v; }
        }
        bits |= (uint64_t)best << (3 * i);
    }
    p[0] = (uint8_t)a0;
    p[1] = (uint8_t)a1;
    for (int i = 0; i < 6; i++) p[2 + i] = (uint8_t)((bits >> (8 * i)) & 0xFF);
}

} /* anonymous namespace */

/* ----------------------------------------------------------- dispatch --- */

int atc_decode(texc_format fmt, const uint8_t *src, size_t src_size,
               uint32_t width, uint32_t height, uint8_t *dst) {
    size_t block_bytes;
    switch (fmt) {
        case TEXC_FORMAT_ATC_RGB:               block_bytes = 8;  break;
        case TEXC_FORMAT_ATC_RGBA_EXPLICIT:
        case TEXC_FORMAT_ATC_RGBA_INTERPOLATED: block_bytes = 16; break;
        default: return TEXC_ERR_UNSUPPORTED;
    }
    uint32_t nbx = (width + 3) / 4, nby = (height + 3) / 4;
    if (src_size < (size_t)nbx * nby * block_bytes)
        return TEXC_ERR_BUFFER_TOO_SMALL;

    uint8_t block[16 * 4];
    for (uint32_t by = 0; by < nby; by++) {
        for (uint32_t bx = 0; bx < nbx; bx++) {
            const uint8_t *p = src + ((size_t)by * nbx + bx) * block_bytes;
            if (fmt == TEXC_FORMAT_ATC_RGB) {
                decode_atc_color_block(p, block);
            } else {
                decode_atc_color_block(p + 8, block);
                if (fmt == TEXC_FORMAT_ATC_RGBA_EXPLICIT)
                    decode_explicit_alpha(p, block);
                else
                    decode_interpolated_alpha(p, block);
            }
            write_block_rgba8(dst, width, height, bx, by, 4, 4, block);
        }
    }
    return TEXC_OK;
}

int atc_encode(texc_format fmt, const uint8_t *src,
               uint32_t width, uint32_t height, uint8_t *dst,
               const texc_encode_options *opts) {
    (void)opts;                     /* no ATC-specific options yet */
    size_t block_bytes;
    switch (fmt) {
        case TEXC_FORMAT_ATC_RGB:               block_bytes = 8;  break;
        case TEXC_FORMAT_ATC_RGBA_EXPLICIT:
        case TEXC_FORMAT_ATC_RGBA_INTERPOLATED: block_bytes = 16; break;
        default: return TEXC_ERR_UNSUPPORTED;
    }
    uint32_t nbx = (width + 3) / 4, nby = (height + 3) / 4;

    uint8_t block[16 * 4];
    for (uint32_t by = 0; by < nby; by++) {
        for (uint32_t bx = 0; bx < nbx; bx++) {
            uint8_t *p = dst + ((size_t)by * nbx + bx) * block_bytes;
            read_block_rgba8(src, width, height, bx, by, 4, 4, block);
            if (fmt == TEXC_FORMAT_ATC_RGB) {
                encode_atc_color_block(block, p);
            } else {
                if (fmt == TEXC_FORMAT_ATC_RGBA_EXPLICIT)
                    encode_explicit_alpha(block, p);
                else
                    encode_interpolated_alpha(block, p);
                encode_atc_color_block(block, p + 8);
            }
        }
    }
    return TEXC_OK;
}

} /* namespace texc */
