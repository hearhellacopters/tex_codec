/*
 * wii.cpp - Nintendo GameCube / Wii GX texture formats (the "TPL" formats).
 *
 * Port of tpl.h from Kerilk/noesis_bayonetta_pc, which is exactly what
 * Project-G1M's PLATFORM::RVL path calls (tplDecodeImage) and which
 * implements the tockdom.com TPL / Image Formats documentation. Project-G1M
 * maps G1T pixel-format IDs onto these in getNWiiFormat():
 *
 *   I4     <- 0x2B, 0x2F                 IA4    <- 0x2D
 *   I8     <- 0x18, 0x28-0x2A, 0x2C, 0x30 IA8    <- 0x26, 0x27, 0x2E
 *   RGB565 <- 0x1C                       RGB5A3 <- 0x25
 *   RGBA8  <- 0x0A, 0x13, 0x22           CMPR   <- 0x10
 *   C4     <- 0x31                       C8     <- 0x32
 *   C14X2  <- 0x15, 0x33
 *
 * Layout facts reproduced exactly from the reference:
 *   - the image is a row-major grid of tiles; every tile is 32 bytes except
 *     RGBA8's 64. Tile size per format: I4 8x8, I8 8x4, IA4 8x4, IA8 4x4,
 *     RGB565 4x4, RGB5A3 4x4, RGBA8 4x4, CMPR 8x8, C4 8x8, C8 8x4,
 *     C14X2 4x4;
 *   - all multi-byte values are big-endian;
 *   - 4bpp formats put the EVEN column in the high nibble;
 *   - channel expansion is v * 255 / max with integer division, which is
 *     NOT bit replication (they differ by 1 for some values);
 *   - RGB5A3: bit 15 set = RGB555 opaque, clear = A3 RGB444;
 *   - RGBA8: a 64-byte tile is a 32-byte AR plane followed by a 32-byte GB
 *     plane, each 4x4 pairs;
 *   - CMPR: an 8x8 tile is four DXT1-style 4x4 sub-blocks in the order
 *     top-left, top-right, bottom-left, bottom-right. Colour words are
 *     big-endian and the 2-bit selectors are packed MSB-first (pixel 0 in
 *     the top two bits - the reverse of BC1). c0 <= c1 selects 3-colour
 *     mode whose 4th entry is fully transparent black;
 *   - paletted formats index a table of big-endian 16-bit entries in IA8,
 *     RGB565 or RGB5A3. C14X2 stores 14-bit indices in 16-bit slots; the
 *     reference does not mask the top two bits, we do (they are documented
 *     as unused, and masking keeps every lookup inside a 32 KB palette).
 *
 * Partial edge tiles are clipped on decode and edge-replicated on encode,
 * which is what tplDecodeImage's last_block_width/height logic amounts to.
 *
 * Cube maps and volume textures are simply several of these 2D images back
 * to back (one per face / slice); decode each with its own dimensions.
 */

#include "codec_common.h"

namespace texc {
namespace {

struct wii_geom {
    uint32_t tw, th;        /* tile size in pixels                       */
    uint32_t bytes;         /* bytes per tile                            */
    uint32_t bpp;           /* bits per pixel (index bits when paletted) */
    bool paletted;
};

bool wii_info(texc_format f, wii_geom *g) {
    switch (f) {
    case TEXC_FORMAT_WII_I4:     *g = { 8, 8, 32,  4, false }; return true;
    case TEXC_FORMAT_WII_I8:     *g = { 8, 4, 32,  8, false }; return true;
    case TEXC_FORMAT_WII_IA4:    *g = { 8, 4, 32,  8, false }; return true;
    case TEXC_FORMAT_WII_IA8:    *g = { 4, 4, 32, 16, false }; return true;
    case TEXC_FORMAT_WII_RGB565: *g = { 4, 4, 32, 16, false }; return true;
    case TEXC_FORMAT_WII_RGB5A3: *g = { 4, 4, 32, 16, false }; return true;
    case TEXC_FORMAT_WII_RGBA8:  *g = { 4, 4, 64, 32, false }; return true;
    case TEXC_FORMAT_WII_CMPR:   *g = { 8, 8, 32,  4, false }; return true;
    case TEXC_FORMAT_WII_C4:     *g = { 8, 8, 32,  4, true  }; return true;
    case TEXC_FORMAT_WII_C8:     *g = { 8, 4, 32,  8, true  }; return true;
    case TEXC_FORMAT_WII_C14X2:  *g = { 4, 4, 32, 16, true  }; return true;
    default:                     return false;
    }
}

/* ------------------------------------------------ value <-> RGBA8 ------ */

/* Reference expansion: v * 255 / max, integer division. */
inline uint8_t ex(uint32_t v, uint32_t max) { return (uint8_t)(v * 255u / max); }
/* Rounded quantisation for the encoders. */
inline uint32_t qn(uint32_t v, uint32_t max) { return (v * max + 127u) / 255u; }

/* Pixel-value kinds shared by the direct formats and the palette entries. */
enum kind { K_I4, K_I8, K_IA4, K_IA8, K_RGB565, K_RGB5A3 };

void unpack(kind k, uint32_t v, uint8_t o[4]) {
    switch (k) {
    case K_I4:
        o[0] = o[1] = o[2] = ex(v & 0xF, 15); o[3] = 255; break;
    case K_I8:
        o[0] = o[1] = o[2] = (uint8_t)v; o[3] = 255; break;
    case K_IA4:
        o[0] = o[1] = o[2] = ex(v & 0xF, 15); o[3] = ex(v >> 4, 15); break;
    case K_IA8:
        o[0] = o[1] = o[2] = (uint8_t)(v & 0xFF); o[3] = (uint8_t)(v >> 8); break;
    case K_RGB565:
        o[0] = ex((v >> 11) & 0x1F, 31);
        o[1] = ex((v >> 5) & 0x3F, 63);
        o[2] = ex(v & 0x1F, 31);
        o[3] = 255;
        break;
    case K_RGB5A3:
        if (v & 0x8000) {
            o[0] = ex((v >> 10) & 0x1F, 31);
            o[1] = ex((v >> 5) & 0x1F, 31);
            o[2] = ex(v & 0x1F, 31);
            o[3] = 255;
        } else {
            o[3] = ex((v >> 12) & 0x7, 7);
            o[0] = ex((v >> 8) & 0xF, 15);
            o[1] = ex((v >> 4) & 0xF, 15);
            o[2] = ex(v & 0xF, 15);
        }
        break;
    }
}

/* Rec.601 luma for the intensity formats. */
inline uint8_t luma(const uint8_t *p) {
    return (uint8_t)((p[0] * 77u + p[1] * 150u + p[2] * 29u + 128u) >> 8);
}

uint32_t pack(kind k, const uint8_t *p) {
    switch (k) {
    case K_I4:     return qn(luma(p), 15);
    case K_I8:     return luma(p);
    case K_IA4:    return (qn(p[3], 15) << 4) | qn(luma(p), 15);
    case K_IA8:    return ((uint32_t)p[3] << 8) | luma(p);
    case K_RGB565: return (qn(p[0], 31) << 11) | (qn(p[1], 63) << 5) | qn(p[2], 31);
    case K_RGB5A3: {
        uint32_t a3 = qn(p[3], 7);
        if (a3 == 7)                     /* fully opaque: use the 555 mode */
            return 0x8000u | (qn(p[0], 31) << 10) | (qn(p[1], 31) << 5) |
                   qn(p[2], 31);
        return (a3 << 12) | (qn(p[0], 15) << 8) | (qn(p[1], 15) << 4) |
               qn(p[2], 15);
    }
    }
    return 0;
}

kind kind_of(texc_format f) {
    switch (f) {
    case TEXC_FORMAT_WII_I4:     return K_I4;
    case TEXC_FORMAT_WII_I8:     return K_I8;
    case TEXC_FORMAT_WII_IA4:    return K_IA4;
    case TEXC_FORMAT_WII_IA8:    return K_IA8;
    case TEXC_FORMAT_WII_RGB565: return K_RGB565;
    default:                     return K_RGB5A3;
    }
}

kind kind_of_palette(texc_palette_format pf) {
    switch (pf) {
    case TEXC_PALETTE_IA8:    return K_IA8;
    case TEXC_PALETTE_RGB565: return K_RGB565;
    default:                  return K_RGB5A3;
    }
}

/* --------------------------------------------- tile texel addressing --- */

/* Read the raw value of texel (row i, column j) of a tile - port of
 * tplComputePixelAddress + tplExtractPixelBits. */
uint32_t read_val(const uint8_t *t, uint32_t tw, uint32_t bpp,
                  uint32_t i, uint32_t j) {
    switch (bpp) {
    case 4: {
        uint8_t b = t[i * (tw >> 1) + (j >> 1)];
        return (j & 1) ? (b & 0xF) : (b >> 4);     /* even column = high */
    }
    case 8:
        return t[i * tw + j];
    default: {
        const uint8_t *p = t + (i * tw + j) * 2;
        return ((uint32_t)p[0] << 8) | p[1];          /* big-endian */
    }
    }
}

void write_val(uint8_t *t, uint32_t tw, uint32_t bpp,
               uint32_t i, uint32_t j, uint32_t v) {
    switch (bpp) {
    case 4: {
        uint8_t &b = t[i * (tw >> 1) + (j >> 1)];
        if (j & 1) b = (uint8_t)((b & 0xF0) | (v & 0xF));
        else       b = (uint8_t)((b & 0x0F) | ((v & 0xF) << 4));
        break;
    }
    case 8:
        t[i * tw + j] = (uint8_t)v;
        break;
    default: {
        uint8_t *p = t + (i * tw + j) * 2;
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    }
    }
}

/* ------------------------------------------------------------- CMPR ---- */

/* Port of tplDecodeCMPRSubBlock: one DXT1-style 4x4 into an 8x8 RGBA tile
 * buffer at (r0, c0). */
void cmpr_sub_decode(const uint8_t *s, uint8_t *tile, uint32_t r0, uint32_t c0) {
    uint32_t c[2] = { ((uint32_t)s[0] << 8) | s[1], ((uint32_t)s[2] << 8) | s[3] };
    int vc[4][4];
    uint8_t px[4];
    for (int k = 0; k < 2; k++) {
        unpack(K_RGB565, c[k], px);
        for (int ch = 0; ch < 4; ch++) vc[k][ch] = px[ch];
    }
    if (c[0] > c[1]) {
        for (int ch = 0; ch < 4; ch++) {
            vc[2][ch] = (vc[1][ch] + 2 * vc[0][ch]) / 3;
            vc[3][ch] = (vc[0][ch] + 2 * vc[1][ch]) / 3;
        }
    } else {
        for (int ch = 0; ch < 4; ch++) {
            vc[2][ch] = (vc[0][ch] + vc[1][ch]) / 2;
            vc[3][ch] = 0;                       /* transparent black */
        }
    }
    for (uint32_t i = 0; i < 4; i++) {
        uint8_t v = s[4 + i];
        for (uint32_t j = 0; j < 4; j++) {
            uint32_t idx = (v >> (6 - 2 * j)) & 3;   /* MSB-first */
            uint8_t *o = tile + ((r0 + i) * 8 + (c0 + j)) * 4;
            o[0] = (uint8_t)vc[idx][0]; o[1] = (uint8_t)vc[idx][1];
            o[2] = (uint8_t)vc[idx][2]; o[3] = (uint8_t)vc[idx][3];
        }
    }
}

/* Reverse the four 2-bit selectors in a byte: BC1 packs pixel 0 in the low
 * bits, CMPR in the high bits. */
inline uint8_t rev2(uint8_t b) {
    return (uint8_t)(((b & 0x03) << 6) | ((b & 0x0C) << 2) |
                     ((b & 0x30) >> 2) | ((b & 0xC0) >> 6));
}

/* Encode one 4x4 (row-major RGBA8) into a CMPR sub-block: run the BC1
 * encoder, then byte-swap the colour words and reverse the selectors. The
 * 3-colour rule (c0 <= c1, 4th entry transparent) is the same in both
 * formats, so punchthrough alpha survives the conversion. */
void cmpr_sub_encode(const uint8_t px[64], uint8_t out[8],
                     const texc_encode_options *opts) {
    uint8_t bc1[8];
    bcn_encode(TEXC_FORMAT_BC1, px, 4, 4, bc1, opts);
    out[0] = bc1[1]; out[1] = bc1[0];
    out[2] = bc1[3]; out[3] = bc1[2];
    for (int i = 0; i < 4; i++) out[4 + i] = rev2(bc1[4 + i]);
}

/* ------------------------------------------------------- per-tile ------ */

void decode_tile(texc_format f, const wii_geom &g, const uint8_t *t,
                 uint8_t *rgba /* tw*th*4, row-major */,
                 const uint8_t *palette, texc_palette_format pf) {
    if (f == TEXC_FORMAT_WII_CMPR) {
        cmpr_sub_decode(t,      rgba, 0, 0);
        cmpr_sub_decode(t + 8,  rgba, 0, 4);
        cmpr_sub_decode(t + 16, rgba, 4, 0);
        cmpr_sub_decode(t + 24, rgba, 4, 4);
        return;
    }
    if (f == TEXC_FORMAT_WII_RGBA8) {
        for (uint32_t i = 0; i < 4; i++)
            for (uint32_t j = 0; j < 4; j++) {
                const uint8_t *ar = t + i * 8 + j * 2;
                const uint8_t *gb = t + 32 + i * 8 + j * 2;
                uint8_t *o = rgba + (i * 4 + j) * 4;
                o[3] = ar[0]; o[0] = ar[1];
                o[1] = gb[0]; o[2] = gb[1];
            }
        return;
    }
    const bool pal = g.paletted;
    const kind k = pal ? kind_of_palette(pf) : kind_of(f);
    for (uint32_t i = 0; i < g.th; i++)
        for (uint32_t j = 0; j < g.tw; j++) {
            uint32_t v = read_val(t, g.tw, g.bpp, i, j);
            if (pal) {
                if (f == TEXC_FORMAT_WII_C14X2) v &= 0x3FFF;
                const uint8_t *e = palette + v * 2;
                v = ((uint32_t)e[0] << 8) | e[1];      /* big-endian entry */
            }
            unpack(k, v, rgba + (i * g.tw + j) * 4);
        }
}

void encode_tile(texc_format f, const wii_geom &g, const uint8_t *rgba,
                 uint8_t *t, const texc_encode_options *opts) {
    memset(t, 0, g.bytes);
    if (f == TEXC_FORMAT_WII_CMPR) {
        static const uint32_t r0[4] = { 0, 0, 4, 4 }, c0[4] = { 0, 4, 0, 4 };
        for (int s = 0; s < 4; s++) {
            uint8_t px[64];
            for (uint32_t i = 0; i < 4; i++)
                memcpy(px + i * 16, rgba + ((r0[s] + i) * 8 + c0[s]) * 4, 16);
            cmpr_sub_encode(px, t + s * 8, opts);
        }
        return;
    }
    if (f == TEXC_FORMAT_WII_RGBA8) {
        for (uint32_t i = 0; i < 4; i++)
            for (uint32_t j = 0; j < 4; j++) {
                const uint8_t *p = rgba + (i * 4 + j) * 4;
                uint8_t *ar = t + i * 8 + j * 2;
                uint8_t *gb = t + 32 + i * 8 + j * 2;
                ar[0] = p[3]; ar[1] = p[0];
                gb[0] = p[1]; gb[1] = p[2];
            }
        return;
    }
    const kind k = kind_of(f);
    for (uint32_t i = 0; i < g.th; i++)
        for (uint32_t j = 0; j < g.tw; j++)
            write_val(t, g.tw, g.bpp, i, j, pack(k, rgba + (i * g.tw + j) * 4));
}

int decode_impl(texc_format f, const uint8_t *src, size_t src_size,
                uint32_t width, uint32_t height, uint8_t *dst,
                const uint8_t *palette, texc_palette_format pf) {
    wii_geom g;
    if (!wii_info(f, &g)) return TEXC_ERR_UNSUPPORTED;
    if (g.paletted && !palette) return TEXC_ERR_NEEDS_PALETTE;

    const uint32_t nx = (width + g.tw - 1) / g.tw;
    const uint32_t ny = (height + g.th - 1) / g.th;
    if (src_size < (size_t)nx * ny * g.bytes) return TEXC_ERR_BUFFER_TOO_SMALL;

    uint8_t tile[8 * 8 * 4];
    for (uint32_t ty = 0; ty < ny; ty++)
        for (uint32_t tx = 0; tx < nx; tx++) {
            const uint8_t *t = src + ((size_t)ty * nx + tx) * g.bytes;
            decode_tile(f, g, t, tile, palette, pf);
            write_block_rgba8(dst, width, height, tx, ty, g.tw, g.th, tile);
        }
    return TEXC_OK;
}

} /* anonymous namespace */

/* ------------------------------------------------------------ dispatch --- */

int wii_decode(texc_format fmt, const uint8_t *src, size_t src_size,
               uint32_t width, uint32_t height, uint8_t *dst) {
    return decode_impl(fmt, src, src_size, width, height, dst,
                       nullptr, TEXC_PALETTE_IA8);
}

int wii_decode_paletted(texc_format fmt, const uint8_t *src, size_t src_size,
                        uint32_t width, uint32_t height,
                        const uint8_t *palette, size_t palette_size,
                        texc_palette_format palette_format, uint8_t *dst) {
    wii_geom g;
    if (!wii_info(fmt, &g) || !g.paletted) return TEXC_ERR_INVALID_ARG;
    if (palette_size < wii_palette_size(fmt)) return TEXC_ERR_BUFFER_TOO_SMALL;
    return decode_impl(fmt, src, src_size, width, height, dst,
                       palette, palette_format);
}

int wii_encode(texc_format fmt, const uint8_t *src,
               uint32_t width, uint32_t height, uint8_t *dst,
               const texc_encode_options *opts) {
    wii_geom g;
    if (!wii_info(fmt, &g)) return TEXC_ERR_UNSUPPORTED;
    if (g.paletted) return TEXC_ERR_UNSUPPORTED;   /* no palette generation */

    const uint32_t nx = (width + g.tw - 1) / g.tw;
    const uint32_t ny = (height + g.th - 1) / g.th;
    uint8_t tile[8 * 8 * 4];
    for (uint32_t ty = 0; ty < ny; ty++)
        for (uint32_t tx = 0; tx < nx; tx++) {
            read_block_rgba8(src, width, height, tx, ty, g.tw, g.th, tile);
            encode_tile(fmt, g, tile, dst + ((size_t)ty * nx + tx) * g.bytes,
                        opts);
        }
    return TEXC_OK;
}

bool wii_is_paletted(texc_format fmt) {
    wii_geom g;
    return wii_info(fmt, &g) && g.paletted;
}

size_t wii_palette_size(texc_format fmt) {
    switch (fmt) {
    case TEXC_FORMAT_WII_C4:    return 16u * 2;       /* 32 bytes     */
    case TEXC_FORMAT_WII_C8:    return 256u * 2;      /* 512 bytes    */
    case TEXC_FORMAT_WII_C14X2: return 16384u * 2;    /* 32768 bytes  */
    default:                    return 0;
    }
}

} /* namespace texc */
