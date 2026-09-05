/*
 * unswizzle.cpp - platform texture tiling <-> linear conversion.
 *
 * Ported from the G1T reference implementations (ref/G1T.h and
 * ref/G1TFormatConvert.h). The generic offset-mapping swizzle machinery
 * there (CustomSwizzle, BlockMxNUnswizzle, the PS4 / D3D12 item tables)
 * is credit to Piken (DwayneR) https://github.com/fdwr
 *
 * Additional sources credited by the reference:
 *   - PS Vita morton order:  xdanieldzd/GXTConvert
 *   - X360 (Xenos) tiling:   bartlomiejduda/ReverseBox
 *   - Wii U GX2 addrlib:     KillzXGaming/Switch-Toolbox (see gx2_addrlib.cpp)
 *   - PS5 layout:            id-daemon RawTex Cooker
 *   - Switch GOB sizing:     "Nintendo Switch Size Code" in ref/G1T.h
 *
 * Conventions used throughout:
 *   - An "element" is one addressing unit of the layout: a compressed block
 *     for block formats, a pixel for RGBA8. Callers supply pixel dimensions;
 *     block geometry is taken from the texc_format.
 *   - The tiled representation covers the platform-padded grid
 *     (texc_swizzled_size bytes). Padding elements are skipped when
 *     converting tiled -> linear and zero-filled when converting
 *     linear -> tiled.
 *   - The reference implements the unswizzle (tiled -> linear) direction;
 *     the swizzle direction here is the exact inverse of the same offset
 *     mapping.
 */

#include "unswizzle.h"
#include "gx2_addrlib.h"

#include <string.h>

namespace texc {

namespace {

/* ------------------------------------------------------------- utilities */

struct Grid {
    uint32_t ew, eh;   /* element grid (blocks across / down)  */
    uint32_t eb;       /* bytes per element                    */
    uint32_t bw, bh;   /* block size in pixels                 */
};

inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
inline uint32_t umin32(uint32_t a, uint32_t b)   { return a < b ? a : b; }

uint32_t next_pow2(uint32_t v)
{
    uint32_t n = 1;
    while (n < v)
        n <<= 1;
    return n;
}

bool get_grid(texc_format fmt, uint32_t w, uint32_t h, Grid *g)
{
    uint32_t bw = 0, bh = 0, bb = 0;
    if (texc_block_dims(fmt, &bw, &bh, &bb) != TEXC_OK || !bw || !bh || !bb)
        return false;
    g->bw = bw;
    g->bh = bh;
    g->eb = bb;
    g->ew = (w + bw - 1) / bw;
    g->eh = (h + bh - 1) / bh;
    return g->ew != 0 && g->eh != 0;
}

/* 16-bit byte swap copy (X360 stores data big-endian; the reference's
 * unswizzle_x360 swaps while untiling). */
void copy_swap16(uint8_t *d, const uint8_t *s, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 1 < n; i += 2) {
        d[i]     = s[i + 1];
        d[i + 1] = s[i];
    }
    if (i < n)
        d[i] = s[i];
}

/* Generic driver for modes expressed as a mapping from a linear element
 * coordinate (x, y) to a byte offset inside the tiled buffer. Only real
 * (non-padding) elements are visited; when swizzling, the tiled buffer is
 * zeroed first so padding is well defined. */
template <typename MapFn>
int run_map(const Grid &g, uint64_t tiled_size,
            const uint8_t *src, size_t src_size,
            uint8_t *dst, size_t dst_size,
            bool to_linear, MapFn map_byte)
{
    const uint64_t lin_size = (uint64_t)g.ew * g.eh * g.eb;
    if (tiled_size > (uint64_t)SIZE_MAX || lin_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;
    if (to_linear) {
        if (src_size < tiled_size || dst_size < lin_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
    } else {
        if (src_size < lin_size || dst_size < tiled_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memset(dst, 0, (size_t)tiled_size);
    }

    for (uint32_t y = 0; y < g.eh; ++y) {
        for (uint32_t x = 0; x < g.ew; ++x) {
            const uint64_t toff = map_byte(x, y);
            const uint64_t loff = ((uint64_t)y * g.ew + x) * g.eb;
            if (toff + g.eb > tiled_size)
                return TEXC_ERR_BUFFER_TOO_SMALL;
            if (to_linear)
                memcpy(dst + (size_t)loff, src + (size_t)toff, g.eb);
            else
                memcpy(dst + (size_t)toff, src + (size_t)loff, g.eb);
        }
    }
    return TEXC_OK;
}

/* --------------------------------------------- PS4 + 3DS: 8x8 Z-order --- */
/* Port of Ps4UnswizzlePixel / DeswizzlePS4Raw (G1TFormatConvert.h):
 * CustomSwizzle items {MortonX,2}x3 interleaved with {MortonY,2}x3, i.e.
 * 8x8 element micro-tiles in row-major tile order with Z-order (Morton,
 * x bit first) inside each tile. G1T.h calls this with pixel dimensions
 * for raw formats and block dimensions + block bytes for BCn - both are
 * "elements" here. The 3DS 8x8 Z-order tile layout is the identical
 * mapping over the element grid.
 * pw is the 8-aligned padded width in elements. */
inline uint64_t map_morton8x8(uint32_t pw, uint32_t x, uint32_t y, uint32_t eb)
{
    const uint32_t m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                       ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return ((uint64_t)(y >> 3) * pw * 8 + (uint64_t)(x >> 3) * 64 + m) * eb;
}

/* ------------------------------------ PS Vita raw: 32x32 pixel tiles ---
 * Port of DeswizzlePSVitaRaw -> SwizzleMasterFunction(Block32x32Unswizzle)
 * (G1TFormatConvert.h). Reference mapping, in PIXEL units:
 *
 *   src = (y / 32) * (width * 32)
 *       + (y % 32) * 32
 *       + (x / 32) * (32 * 32)
 *       + (x % 32);
 *
 * Two reference details that matter and are reproduced exactly:
 *   - the RAW width is used, NOT a width rounded up to whole 32x32 tiles,
 *     so the tiled buffer is exactly width * height * bytesPerPixel - the
 *     size G1T stores (currentImageSize = w*h*bpp/8);
 *   - a mapped offset that lands outside the image is SKIPPED rather than
 *     treated as an error (the reference guards with
 *     `sourceIndex < sourcePixelCount`). That happens whenever a dimension
 *     is not a multiple of 32, and means the mapping is not a bijection
 *     there: those texels stay zero and a reswizzle cannot restore them.
 */
inline uint64_t map_vita_raw_px(uint32_t w, uint32_t x, uint32_t y)
{
    return (uint64_t)(y >> 5) * ((uint64_t)w * 32) + (uint64_t)(y & 31) * 32 +
           (uint64_t)(x >> 5) * 1024 + (x & 31);
}

/* Bytes per pixel for the raw Vita path: `arg` overrides the format's own
 * element size, mirroring the reference's bitsPerPixel parameter (G1T uses
 * 8/16/24/32bpp raw Vita textures; the comment at the call site notes 24bpp
 * specifically). 0 = use the format's size. Returns 0 if unusable. */
inline uint32_t vita_raw_bpp(const Grid &g, uint32_t arg)
{
    const uint32_t eb = arg ? arg : g.eb;
    return (eb >= 1 && eb <= 16) ? eb : 0;
}

int convert_vita_raw(const Grid &g, uint32_t arg,
                     const uint8_t *src, size_t src_size,
                     uint8_t *dst, size_t dst_size, bool to_linear)
{
    const uint32_t bpp = vita_raw_bpp(g, arg);
    if (!bpp) return TEXC_ERR_INVALID_ARG;
    const uint64_t pixels = (uint64_t)g.ew * g.eh;
    const uint64_t bytes = pixels * bpp;
    if (bytes > (uint64_t)SIZE_MAX) return TEXC_ERR_INVALID_ARG;
    if (src_size < bytes || dst_size < bytes) return TEXC_ERR_BUFFER_TOO_SMALL;

    memset(dst, 0, (size_t)bytes);       /* unmapped texels stay zero */
    for (uint32_t y = 0; y < g.eh; ++y) {
        for (uint32_t x = 0; x < g.ew; ++x) {
            const uint64_t sw = map_vita_raw_px(g.ew, x, y);
            if (sw >= pixels) continue;  /* reference skips out-of-range */
            const uint64_t lin = (uint64_t)y * g.ew + x;
            if (to_linear)
                memcpy(dst + (size_t)(lin * bpp), src + (size_t)(sw * bpp), bpp);
            else
                memcpy(dst + (size_t)(sw * bpp), src + (size_t)(lin * bpp), bpp);
        }
    }
    return TEXC_OK;
}

/* --------------------------------------- PSP: 16-byte x 8-row tiles ----- */
/* Block16x8-style tiling ("Predator PSP swizzle" in the reference) applied
 * at byte granularity, which is the native PSP GE layout: the row of bytes
 * is split into 16-byte columns, 8 rows tall. prow = 16-aligned row bytes. */
inline uint64_t map_psp(uint32_t prow, uint32_t xb, uint32_t y)
{
    return (uint64_t)(y >> 3) * prow * 8 + (uint64_t)(xb >> 4) * 128 +
           (uint64_t)(y & 7) * 16 + (xb & 15);
}

/* --------------------------- D3D12 64KB raw: port of D3D12_64KBSwizzleRaw */
/* CustomSwizzle items {MortonX,128},{MortonY,128},{MortonX,16} evaluated in
 * closed form for pw a multiple of 128. */
inline uint64_t map_d3d12_raw(uint32_t pw, uint32_t x, uint32_t y, uint32_t eb)
{
    const uint64_t sw = (uint64_t)(x % 128) + (uint64_t)(y % 128) * 128 +
                        (uint64_t)((x / 128) % 16) * 16384 +
                        (uint64_t)(x / 2048) * 262144;
    return ((uint64_t)(y / 128) * 128 * pw + sw) * eb;
}

/* ------------------------------------------- Switch (Tegra X1) GOBs ----- */
/* 64-byte x 8-row GOBs stacked into blocks of `bh` GOBs. Matches
 * swizzled_mip_size() in ref/G1T.h and the layout consumed by
 * Image_UntileBlockLinearGOBs. xb = byte offset within the row of blocks. */
inline uint64_t map_switch_gob(uint32_t width_gobs, uint32_t bh,
                               uint32_t xb, uint32_t y)
{
    uint64_t a = (uint64_t)(y / (8 * bh)) * 512ull * bh * width_gobs +
                 (uint64_t)(xb / 64) * 512ull * bh +
                 (uint64_t)((y % (8 * bh)) / 8) * 512;
    a += ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
         ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
    return a;
}

/* Port of block_height_enum() / mip_block_height() from ref/G1T.h. */
uint32_t switch_auto_block_height(uint32_t blocks_y)
{
    const uint32_t h = blocks_y + blocks_y / 2;
    uint32_t bh;
    if (h >= 128)     bh = 16;
    else if (h >= 64) bh = 8;
    else if (h >= 32) bh = 4;
    else if (h >= 16) bh = 2;
    else              bh = 1;
    while (blocks_y <= (bh / 2) * 8 && bh > 1)
        bh /= 2;
    return bh;
}

bool switch_block_height(uint32_t blocks_y, uint32_t arg, uint32_t *bh)
{
    if (arg == 0xFFFFFFFFu) {
        *bh = switch_auto_block_height(blocks_y);
        return true;
    }
    if (arg > 5)
        return false;
    *bh = 1u << arg;
    return true;
}

/* ------------------------------------------------- X360 (Xenos) tiling -- */
/* Port of xg_address_2d_tiled_x/y from the reference ("X360 SWIZZLE CODE",
 * source bartlomiejduda/ReverseBox). Maps a tiled element index to its
 * linear (x, y). */
uint32_t xg_log_bpp(uint32_t texel_pitch)
{
    return (texel_pitch >> 2) + ((texel_pitch >> 1) >> (texel_pitch >> 2));
}

uint32_t xg_tiled_x(uint32_t block_offset, uint32_t width_in_blocks,
                    uint32_t texel_pitch)
{
    const uint32_t aligned_width = (width_in_blocks + 31) & ~31u;
    const uint32_t log_bpp = xg_log_bpp(texel_pitch);
    const uint32_t offset_byte = block_offset << log_bpp;
    const uint32_t offset_tile = ((offset_byte & ~0xFFFu) >> 3) +
                                 ((offset_byte & 0x700u) >> 2) +
                                 (offset_byte & 0x3Fu);
    const uint32_t offset_macro = offset_tile >> (7 + log_bpp);

    const uint32_t macro_x = (offset_macro % (aligned_width >> 5)) << 2;
    const uint32_t tile = (((offset_tile >> (5 + log_bpp)) & 2) +
                           (offset_byte >> 6)) & 3;
    const uint32_t macro = (macro_x + tile) << 3;
    const uint32_t micro = ((((offset_tile >> 1) & ~0xFu) + (offset_tile & 0xFu)) &
                            ((texel_pitch << 3) - 1)) >> log_bpp;
    return macro + micro;
}

uint32_t xg_tiled_y(uint32_t block_offset, uint32_t width_in_blocks,
                    uint32_t texel_pitch)
{
    const uint32_t aligned_width = (width_in_blocks + 31) & ~31u;
    const uint32_t log_bpp = xg_log_bpp(texel_pitch);
    const uint32_t offset_byte = block_offset << log_bpp;
    const uint32_t offset_tile = ((offset_byte & ~0xFFFu) >> 3) +
                                 ((offset_byte & 0x700u) >> 2) +
                                 (offset_byte & 0x3Fu);
    const uint32_t offset_macro = offset_tile >> (7 + log_bpp);

    const uint32_t macro_y = (offset_macro / (aligned_width >> 5)) << 2;
    const uint32_t tile = ((offset_tile >> (6 + log_bpp)) & 1) +
                          ((offset_byte & 0x800u) >> 10);
    const uint32_t macro = (macro_y + tile) << 3;
    const uint32_t micro = (((offset_tile & (((texel_pitch << 6) - 1) & ~0x1Fu)) +
                             ((offset_tile & 0xFu) << 1)) >> (3 + log_bpp)) & ~1u;
    return macro + micro + ((offset_tile & 0x10u) >> 4);
}

/* Derive the X360 texel grid: `arg` overrides the texel byte pitch (the
 * addressing granularity), default is the format's block bytes, matching
 * unswizzle_x360(dst, src, size, w, h, blockWidth, minBytes) in G1T.h. */
bool x360_texel_grid(const Grid &g, uint32_t arg,
                     uint32_t *pitch, uint32_t *gw, uint32_t *gh)
{
    uint32_t p = arg ? arg : g.eb;
    if (p == 0 || p > 16 || (p & (p - 1)) != 0)
        return false;
    const uint64_t row_bytes = (uint64_t)g.ew * g.eb;
    if (row_bytes % p)
        return false;
    *pitch = p;
    *gw = (uint32_t)(row_bytes / p);
    *gh = g.eh;
    return true;
}

int convert_x360(const Grid &g, uint32_t arg,
                 const uint8_t *src, size_t src_size,
                 uint8_t *dst, size_t dst_size, bool to_linear)
{
    uint32_t p = 0, gw = 0, gh = 0;
    if (!x360_texel_grid(g, arg, &p, &gw, &gh))
        return TEXC_ERR_INVALID_ARG;

    const uint32_t pw = align_up(gw, 32);
    const uint32_t ph = align_up(gh, 32);
    const uint64_t tiled_size = (uint64_t)pw * ph * p;
    const uint64_t lin_size = (uint64_t)gw * gh * p;
    if (tiled_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;

    if (to_linear) {
        if (src_size < tiled_size || dst_size < lin_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
    } else {
        if (src_size < lin_size || dst_size < tiled_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memset(dst, 0, (size_t)tiled_size);
    }

    const uint64_t total = (uint64_t)pw * ph;
    for (uint64_t t = 0; t < total; ++t) {
        const uint32_t x = xg_tiled_x((uint32_t)t, pw, p);
        const uint32_t y = xg_tiled_y((uint32_t)t, pw, p);
        if (x >= gw || y >= gh)
            continue; /* padding element */
        const uint64_t toff = t * p;
        const uint64_t loff = ((uint64_t)y * gw + x) * p;
        /* The reference byte-swaps 16-bit words while untiling (Xenos data
         * is big-endian); the inverse re-swaps. */
        if (to_linear)
            copy_swap16(dst + (size_t)loff, src + (size_t)toff, p);
        else
            copy_swap16(dst + (size_t)toff, src + (size_t)loff, p);
    }
    return TEXC_OK;
}

/* ------------------------------------------ PS Vita block-format morton - */
/* Port of convert_morton_psvita_dreamcast (source xdanieldzd/GXTConvert).
 * The tiled stream enumerates blocks in Morton order with the linear
 * leftover scheme for non-square surfaces; the block grid is padded to
 * powers of two (GXM allocates pow2 surfaces), which also makes the map a
 * bijection. For power-of-two images this is byte-identical to the
 * reference. */
int compact1by1(int v)
{
    v &= 0x55555555;
    v = (v ^ (v >> 1)) & 0x33333333;
    v = (v ^ (v >> 2)) & 0x0F0F0F0F;
    v = (v ^ (v >> 4)) & 0x00FF00FF;
    v = (v ^ (v >> 8)) & 0x0000FFFF;
    return v;
}

int convert_vita_bc(const Grid &g,
                    const uint8_t *src, size_t src_size,
                    uint8_t *dst, size_t dst_size, bool to_linear)
{
    const uint32_t pw = next_pow2(g.ew);
    const uint32_t ph = next_pow2(g.eh);
    const uint64_t tiled_size = (uint64_t)pw * ph * g.eb;
    const uint64_t lin_size = (uint64_t)g.ew * g.eh * g.eb;
    if (tiled_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;

    if (to_linear) {
        if (src_size < tiled_size || dst_size < lin_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
    } else {
        if (src_size < lin_size || dst_size < tiled_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memset(dst, 0, (size_t)tiled_size);
    }

    const uint32_t mn = umin32(pw, ph);
    uint32_t k = 0;
    while ((1u << (k + 1)) <= mn)
        ++k;

    const uint64_t total = (uint64_t)pw * ph;
    for (uint64_t tt = 0; tt < total; ++tt) {
        const int t = (int)tt;
        uint32_t x, y;
        if (ph < pw) {
            /* XXXyxyxyx -> XXXxxxyyy */
            const int j = (t >> (2 * k) << (2 * k)) |
                          ((compact1by1(t >> 1) & (int)(mn - 1)) << k) |
                          (compact1by1(t) & (int)(mn - 1));
            x = (uint32_t)j / ph;
            y = (uint32_t)j % ph;
        } else {
            /* YYYyxyxyx -> YYYyyyxxx */
            const int j = (t >> (2 * k) << (2 * k)) |
                          ((compact1by1(t) & (int)(mn - 1)) << k) |
                          (compact1by1(t >> 1) & (int)(mn - 1));
            x = (uint32_t)j % pw;
            y = (uint32_t)j / pw;
        }

        if (x >= g.ew || y >= g.eh)
            continue;

        const uint64_t toff = tt * g.eb;
        const uint64_t loff = ((uint64_t)y * g.ew + x) * g.eb;
        if (to_linear)
            memcpy(dst + (size_t)loff, src + (size_t)toff, g.eb);
        else
            memcpy(dst + (size_t)toff, src + (size_t)loff, g.eb);
    }
    return TEXC_OK;
}

/* ------------------------------------------------------------- PS5 ----- */
/* Port of UnswizzlePS5 / PS5morton (source id-daemon RawTex Cooker).
 * The tiled data is a sequential stream: 128x128-element macro tiles for
 * raw formats, 64x64-block macro tiles for block formats, each filled in
 * the nested morton/sub-tile order below. */
int ps5_morton(int t, int sx, int sy)
{
    int xw = 1, yw = 1;
    int v = t, rx = sx, ry = sy, x = 0, y = 0;
    while (rx > 1 || ry > 1) {
        if (rx > 1) {
            x += xw * (v & 1);
            v >>= 1;
            xw *= 2;
            rx >>= 1;
        }
        if (ry > 1) {
            y += yw * (v & 1);
            v >>= 1;
            yw *= 2;
            ry >>= 1;
        }
    }
    return y * sx + x;
}

uint64_t ps5_tiled_size(const Grid &g)
{
    if (g.bw == 1) {
        const uint64_t tx = (g.ew + 127) / 128, ty = (g.eh + 127) / 128;
        return tx * ty * 16384ull * g.eb;
    }
    if (g.eb != 16 && g.eb != 8 && g.eb != 4)
        return 0;
    const uint64_t tx = (g.ew + 63) / 64, ty = (g.eh + 63) / 64;
    return tx * ty * 4096ull * g.eb;
}

int convert_ps5(const Grid &g,
                const uint8_t *src, size_t src_size,
                uint8_t *dst, size_t dst_size, bool to_linear)
{
    const uint64_t tiled_size = ps5_tiled_size(g);
    const uint64_t lin_size = (uint64_t)g.ew * g.eh * g.eb;
    if (tiled_size == 0)
        return TEXC_ERR_UNSUPPORTED;
    if (tiled_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;

    if (to_linear) {
        if (src_size < tiled_size || dst_size < lin_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
    } else {
        if (src_size < lin_size || dst_size < tiled_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memset(dst, 0, (size_t)tiled_size);
    }

    const uint32_t eb = g.eb;
    uint64_t stream = 0;

    auto emit = [&](uint32_t x, uint32_t y) {
        if (x < g.ew && y < g.eh) {
            const uint64_t loff = ((uint64_t)y * g.ew + x) * eb;
            if (to_linear)
                memcpy(dst + (size_t)loff, src + (size_t)stream, eb);
            else
                memcpy(dst + (size_t)stream, src + (size_t)loff, eb);
        }
        stream += eb;
    };

    if (g.bw == 1) {
        /* Raw path: 128x128 pixel tiles, 32x16 morton of 4x8 sub-tiles. */
        const uint32_t tiles_y = (g.eh + 127) / 128;
        const uint32_t tiles_x = (g.ew + 127) / 128;
        for (uint32_t h = 0; h < tiles_y; ++h)
            for (uint32_t w = 0; w < tiles_x; ++w)
                for (int t = 0; t < 512; ++t) {
                    const int mi = ps5_morton(t, 32, 16);
                    const uint32_t mw = (uint32_t)(mi % 32);
                    const uint32_t mh = (uint32_t)(mi / 32);
                    for (uint32_t sub = 0; sub < 32; ++sub)
                        emit(w * 128 + mw * 4 + sub % 4,
                             h * 128 + mh * 8 + sub / 4);
                }
    } else {
        /* Block path: 64x64 block tiles; divisor selected by block size
         * exactly as the reference (16B -> 1, 8B -> 2, 4B -> 4). */
        const uint32_t div = (eb == 16) ? 1u : (eb == 8) ? 2u : 4u;
        const uint32_t tiles_y = (g.eh + 63) / 64;
        const uint32_t tiles_x = (g.ew + 63) / 64;
        for (uint32_t h = 0; h < tiles_y; ++h)
            for (uint32_t w = 0; w < tiles_x; ++w)
                for (uint32_t t = 0; t < 256 / div; ++t) {
                    const int mi = ps5_morton((int)t, 16, (int)(16 / div));
                    const uint32_t mw = (uint32_t)(mi / 16);
                    const uint32_t mh = (uint32_t)(mi % 16);
                    for (uint32_t s1 = 0; s1 < 16; ++s1)
                        for (uint32_t s2 = 0; s2 < div; ++s2)
                            emit(w * 64 + (mw * 4 + s1 / 4) * div + s2,
                                 h * 64 + mh * 4 + s1 % 4);
                }
    }
    return TEXC_OK;
}

/* ------------------------------------------------ D3D12 64KB (BCn) ------ */
/* Port of DeswizzleD3D12_64KBDXT: 64KB tiles in row-major tile order; each
 * tile holds 64 rows of 1024 bytes (1024 / block_bytes blocks per row).
 * Data of 64KB or less is stored linearly (see the currentImageSize > 65536
 * check in G1T.h) - handled by the caller. */
int convert_dx12_bc(const Grid &g,
                    const uint8_t *src, size_t src_size,
                    uint8_t *dst, size_t dst_size, bool to_linear)
{
    const uint32_t tile_w = 1024 / g.eb;
    const uint32_t tiles_x = (g.ew + tile_w - 1) / tile_w;
    const uint32_t tiles_y = (g.eh + 63) / 64;
    const uint64_t tiled_size = (uint64_t)tiles_x * tiles_y * 65536u;
    const uint64_t lin_size = (uint64_t)g.ew * g.eh * g.eb;
    if (tiled_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;

    if (to_linear) {
        if (src_size < tiled_size || dst_size < lin_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
    } else {
        if (src_size < lin_size || dst_size < tiled_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memset(dst, 0, (size_t)tiled_size);
    }

    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            const uint64_t tile_base = ((uint64_t)ty * tiles_x + tx) * 65536u;
            for (uint32_t row = 0; row < 64; ++row) {
                const uint32_t y = ty * 64 + row;
                if (y >= g.eh)
                    continue;
                const uint32_t x_off = tx * tile_w;
                if (x_off >= g.ew)
                    continue;
                const uint32_t n =
                    umin32(tile_w, g.ew - x_off) * g.eb;
                const uint64_t toff = tile_base + (uint64_t)row * 1024;
                const uint64_t loff = ((uint64_t)y * g.ew + x_off) * g.eb;
                if (to_linear)
                    memcpy(dst + (size_t)loff, src + (size_t)toff, n);
                else
                    memcpy(dst + (size_t)toff, src + (size_t)loff, n);
            }
        }
    }
    return TEXC_OK;
}

/* ------------------------------------------------------------ Wii U ----- */
/* GX2 2D_TILED_THIN1 via the embedded addrlib subset (gx2_addrlib.cpp),
 * matching Gx2Decode/swizzleSurf in the reference for mip 0, slice 0.
 * `arg` is the GX2 swizzle value from the texture header (usually 0):
 * pipe = (arg >> 8) & 1, bank = (arg >> 9) & 3. */
bool wiiu_hw_format(const Grid &g, uint32_t *hw)
{
    if (g.bw == 4 && g.bh == 4 && g.eb == 8)  { *hw = 0x31; return true; }
    if (g.bw == 4 && g.bh == 4 && g.eb == 16) { *hw = 0x33; return true; }
    if (g.bw == 1 && g.bh == 1 && g.eb == 4)  { *hw = 0x1A; return true; }
    return false; /* no GX2 hardware layout for this block geometry */
}

bool wiiu_surface(const Grid &g, uint32_t w, uint32_t h, gx2::SurfaceInfo *si)
{
    uint32_t hw = 0;
    if (!wiiu_hw_format(g, &hw))
        return false;
    /* Tile mode 4 (ADDR_TM_2D_TILED_THIN1) - the G1T default. */
    return gx2::compute_surface_info_mip0(hw, w, h, 4, si);
}

int convert_wiiu(const Grid &g, uint32_t w, uint32_t h, uint32_t arg,
                 const uint8_t *src, size_t src_size,
                 uint8_t *dst, size_t dst_size, bool to_linear)
{
    gx2::SurfaceInfo si;
    if (!wiiu_surface(g, w, h, &si))
        return TEXC_ERR_UNSUPPORTED;
    if (si.bpp / 8 != g.eb)
        return TEXC_ERR_UNSUPPORTED;

    const uint32_t pipe_swz = (arg >> 8) & 1;
    const uint32_t bank_swz = (arg >> 9) & 3;
    const uint32_t pitch = si.pitch;
    const uint32_t sheight = si.height;
    const uint32_t tile_mode = si.tile_mode;
    const uint32_t bpp = si.bpp;

    return run_map(g, si.surf_size, src, src_size, dst, dst_size, to_linear,
                   [&](uint32_t x, uint32_t y) -> uint64_t {
                       return (uint64_t)gx2::addr_from_coord(
                           x, y, bpp, pitch, sheight, tile_mode,
                           pipe_swz, bank_swz);
                   });
}

/* ----------------------------------------------- tiled size per mode ---- */

uint64_t tiled_size_impl(texc_swizzle_mode mode, const Grid &g,
                         uint32_t w, uint32_t h, uint32_t arg)
{
    const uint64_t lin = (uint64_t)g.ew * g.eh * g.eb;

    switch (mode) {
    case TEXC_SWIZZLE_NONE:
        return lin;

    case TEXC_SWIZZLE_PS4:
    case TEXC_SWIZZLE_3DS:
        return (uint64_t)align_up(g.ew, 8) * align_up(g.eh, 8) * g.eb;

    case TEXC_SWIZZLE_PS5:
        return ps5_tiled_size(g);

    case TEXC_SWIZZLE_SWITCH: {
        uint32_t bh = 0;
        if (!switch_block_height(g.eh, arg, &bh))
            return 0;
        const uint64_t row_bytes = (uint64_t)g.ew * g.eb;
        const uint64_t width_gobs = (row_bytes + 63) / 64;
        const uint64_t height_blocks = ((uint64_t)g.eh + bh * 8 - 1) / (bh * 8);
        return width_gobs * height_blocks * bh * 512ull;
    }

    case TEXC_SWIZZLE_PSVITA:
        if (g.bw == 1) {
            /* raw: DeswizzlePSVitaRaw works on exactly w*h*bpp bytes */
            const uint32_t bpp = vita_raw_bpp(g, arg);
            return bpp ? (uint64_t)g.ew * g.eh * bpp : 0;
        }
        return (uint64_t)next_pow2(g.ew) * next_pow2(g.eh) * g.eb;

    case TEXC_SWIZZLE_X360: {
        uint32_t p = 0, gw = 0, gh = 0;
        if (!x360_texel_grid(g, arg, &p, &gw, &gh))
            return 0;
        return (uint64_t)align_up(gw, 32) * align_up(gh, 32) * p;
    }

    case TEXC_SWIZZLE_PSP: {
        const uint32_t row_bytes = g.ew * g.eb;
        return (uint64_t)align_up(row_bytes, 16) * align_up(g.eh, 8);
    }

    case TEXC_SWIZZLE_WIIU: {
        gx2::SurfaceInfo si;
        if (!wiiu_surface(g, w, h, &si))
            return 0;
        return si.surf_size;
    }

    case TEXC_SWIZZLE_DX12_64KB: {
        /* Data of 64KB or less is not swizzled (reference behaviour). */
        if (lin <= 65536)
            return lin;
        if (g.bw > 1) {
            const uint32_t tile_w = 1024 / g.eb;
            const uint64_t tx = ((uint64_t)g.ew + tile_w - 1) / tile_w;
            const uint64_t ty = ((uint64_t)g.eh + 63) / 64;
            return tx * ty * 65536ull;
        }
        return (uint64_t)align_up(g.ew, 128) * align_up(g.eh, 128) * g.eb;
    }

    default:
        return 0;
    }
}

} /* anonymous namespace */

/* ===================================================== public interface = */

size_t linear_size(texc_swizzle_mode mode, texc_format fmt,
                   uint32_t width, uint32_t height, uint32_t arg)
{
    if (mode < TEXC_SWIZZLE_NONE || mode >= TEXC_SWIZZLE_MODE_COUNT)
        return 0;
    if (width == 0 || height == 0)
        return 0;
    Grid g;
    if (!get_grid(fmt, width, height, &g))
        return 0;

    /* PS Vita raw is the one layout whose element size can be overridden
     * (the reference's bitsPerPixel), so the linear side is w*h*bpp rather
     * than the format's own encoded size. */
    uint32_t eb = g.eb;
    if (mode == TEXC_SWIZZLE_PSVITA && g.bw == 1) {
        eb = vita_raw_bpp(g, arg);
        if (!eb)
            return 0;
    }
    const uint64_t sz = (uint64_t)g.ew * g.eh * eb;
    return sz > (uint64_t)SIZE_MAX ? 0 : (size_t)sz;
}

size_t swizzled_size(texc_swizzle_mode mode, texc_format fmt,
                     uint32_t width, uint32_t height, uint32_t arg)
{
    if (mode < TEXC_SWIZZLE_NONE || mode >= TEXC_SWIZZLE_MODE_COUNT)
        return 0;
    if (width == 0 || height == 0)
        return 0;
    Grid g;
    if (!get_grid(fmt, width, height, &g))
        return 0;
    const uint64_t sz = tiled_size_impl(mode, g, width, height, arg);
    if (sz == 0 || sz > (uint64_t)SIZE_MAX)
        return 0;
    return (size_t)sz;
}

int swizzle_convert(texc_swizzle_mode mode, texc_format fmt,
                    uint32_t width, uint32_t height,
                    const uint8_t *src, size_t src_size,
                    uint8_t *dst, size_t dst_size,
                    uint32_t arg, bool dir_to_linear)
{
    if (mode < TEXC_SWIZZLE_NONE || mode >= TEXC_SWIZZLE_MODE_COUNT)
        return TEXC_ERR_INVALID_ARG;
    if (!src || !dst || width == 0 || height == 0)
        return TEXC_ERR_INVALID_ARG;

    Grid g;
    if (!get_grid(fmt, width, height, &g))
        return TEXC_ERR_INVALID_ARG;

    const uint64_t lin = (uint64_t)g.ew * g.eh * g.eb;
    if (lin > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;

    switch (mode) {
    case TEXC_SWIZZLE_NONE: {
        if (src_size < lin || dst_size < lin)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memcpy(dst, src, (size_t)lin);
        return TEXC_OK;
    }

    case TEXC_SWIZZLE_PS4:
    case TEXC_SWIZZLE_3DS: {
        const uint64_t ts = tiled_size_impl(mode, g, width, height, arg);
        const uint32_t pw = align_up(g.ew, 8);
        return run_map(g, ts, src, src_size, dst, dst_size, dir_to_linear,
                       [&](uint32_t x, uint32_t y) {
                           return map_morton8x8(pw, x, y, g.eb);
                       });
    }

    case TEXC_SWIZZLE_PS5:
        return convert_ps5(g, src, src_size, dst, dst_size, dir_to_linear);

    case TEXC_SWIZZLE_SWITCH: {
        uint32_t bh = 0;
        if (!switch_block_height(g.eh, arg, &bh))
            return TEXC_ERR_INVALID_ARG;
        const uint64_t ts = tiled_size_impl(mode, g, width, height, arg);
        const uint32_t width_gobs =
            (uint32_t)(((uint64_t)g.ew * g.eb + 63) / 64);
        return run_map(g, ts, src, src_size, dst, dst_size, dir_to_linear,
                       [&](uint32_t x, uint32_t y) {
                           return map_switch_gob(width_gobs, bh, x * g.eb, y);
                       });
    }

    case TEXC_SWIZZLE_PSVITA:
        if (g.bw == 1)
            return convert_vita_raw(g, arg, src, src_size, dst, dst_size,
                                    dir_to_linear);
        return convert_vita_bc(g, src, src_size, dst, dst_size, dir_to_linear);

    case TEXC_SWIZZLE_X360:
        return convert_x360(g, arg, src, src_size, dst, dst_size, dir_to_linear);

    case TEXC_SWIZZLE_PSP: {
        const uint64_t ts = tiled_size_impl(mode, g, width, height, arg);
        const uint32_t prow = align_up(g.ew * g.eb, 16);
        return run_map(g, ts, src, src_size, dst, dst_size, dir_to_linear,
                       [&](uint32_t x, uint32_t y) {
                           return map_psp(prow, x * g.eb, y);
                       });
    }

    case TEXC_SWIZZLE_WIIU:
        return convert_wiiu(g, width, height, arg, src, src_size, dst,
                            dst_size, dir_to_linear);

    case TEXC_SWIZZLE_DX12_64KB: {
        if (lin <= 65536) {
            /* Not swizzled below/at 64KB (reference behaviour). */
            if (src_size < lin || dst_size < lin)
                return TEXC_ERR_BUFFER_TOO_SMALL;
            memcpy(dst, src, (size_t)lin);
            return TEXC_OK;
        }
        if (g.bw > 1)
            return convert_dx12_bc(g, src, src_size, dst, dst_size,
                                   dir_to_linear);
        const uint64_t ts = tiled_size_impl(mode, g, width, height, arg);
        const uint32_t pw = align_up(g.ew, 128);
        return run_map(g, ts, src, src_size, dst, dst_size, dir_to_linear,
                       [&](uint32_t x, uint32_t y) {
                           return map_d3d12_raw(pw, x, y, g.eb);
                       });
    }

    default:
        return TEXC_ERR_UNSUPPORTED;
    }
}

} /* namespace texc */

/* ========================================================== self test === */
#ifdef TEXC_SELFTEST

#include <stdio.h>
#include <stdlib.h>
#include <vector>

/* Standalone build: provide texc_block_dims locally (normally supplied by
 * tex_codec.cpp). */
extern "C" int texc_block_dims(texc_format format, uint32_t *bw,
                               uint32_t *bh, uint32_t *bb)
{
    uint32_t w = 4, h = 4, b = 16;
    switch (format) {
    case TEXC_FORMAT_RGBA8:            w = 1; h = 1; b = 4;  break;
    case TEXC_FORMAT_BC1:
    case TEXC_FORMAT_BC4:
    case TEXC_FORMAT_BC4_SNORM:
    case TEXC_FORMAT_ETC1_RGB:
    case TEXC_FORMAT_ETC2_RGB:
    case TEXC_FORMAT_ETC2_RGBA1:
    case TEXC_FORMAT_EAC_R11:
    case TEXC_FORMAT_EAC_R11_SIGNED:
    case TEXC_FORMAT_ATC_RGB:          b = 8;                break;
    case TEXC_FORMAT_BC2:
    case TEXC_FORMAT_BC3:
    case TEXC_FORMAT_BC5:
    case TEXC_FORMAT_BC5_SNORM:
    case TEXC_FORMAT_BC6H_UF16:
    case TEXC_FORMAT_BC6H_SF16:
    case TEXC_FORMAT_BC7:
    case TEXC_FORMAT_ETC2_RGBA8:
    case TEXC_FORMAT_EAC_RG11:
    case TEXC_FORMAT_EAC_RG11_SIGNED:
    case TEXC_FORMAT_ATC_RGBA_EXPLICIT:
    case TEXC_FORMAT_ATC_RGBA_INTERPOLATED: b = 16;          break;
    case TEXC_FORMAT_PVRTC1_2BPP_RGB:
    case TEXC_FORMAT_PVRTC1_2BPP_RGBA:
    case TEXC_FORMAT_PVRTC2_2BPP:      w = 8; h = 4; b = 8;  break;
    case TEXC_FORMAT_PVRTC1_4BPP_RGB:
    case TEXC_FORMAT_PVRTC1_4BPP_RGBA:
    case TEXC_FORMAT_PVRTC2_4BPP:      w = 4; h = 4; b = 8;  break;
    case TEXC_FORMAT_ASTC_4x4:         w = 4;  h = 4;        break;
    case TEXC_FORMAT_ASTC_5x4:         w = 5;  h = 4;        break;
    case TEXC_FORMAT_ASTC_5x5:         w = 5;  h = 5;        break;
    case TEXC_FORMAT_ASTC_6x5:         w = 6;  h = 5;        break;
    case TEXC_FORMAT_ASTC_6x6:         w = 6;  h = 6;        break;
    case TEXC_FORMAT_ASTC_8x5:         w = 8;  h = 5;        break;
    case TEXC_FORMAT_ASTC_8x6:         w = 8;  h = 6;        break;
    case TEXC_FORMAT_ASTC_8x8:         w = 8;  h = 8;        break;
    case TEXC_FORMAT_ASTC_10x5:        w = 10; h = 5;        break;
    case TEXC_FORMAT_ASTC_10x6:        w = 10; h = 6;        break;
    case TEXC_FORMAT_ASTC_10x8:        w = 10; h = 8;        break;
    case TEXC_FORMAT_ASTC_10x10:       w = 10; h = 10;       break;
    case TEXC_FORMAT_ASTC_12x10:       w = 12; h = 10;       break;
    case TEXC_FORMAT_ASTC_12x12:       w = 12; h = 12;       break;
    default:
        return TEXC_ERR_INVALID_ARG;
    }
    if (bw) *bw = w;
    if (bh) *bh = h;
    if (bb) *bb = b;
    return TEXC_OK;
}

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

size_t format_linear_size(texc_format fmt, uint32_t w, uint32_t h)
{
    uint32_t bw, bh, bb;
    texc_block_dims(fmt, &bw, &bh, &bb);
    return (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bb;
}

bool roundtrip(texc_swizzle_mode mode, const char *mname,
               texc_format fmt, const char *fname,
               uint32_t w, uint32_t h, uint32_t arg)
{
    char label[160];
    const size_t lin = format_linear_size(fmt, w, h);
    const size_t tiled = texc::swizzled_size(mode, fmt, w, h, arg);

    snprintf(label, sizeof label, "%s %s %ux%u arg=%08x swizzled_size",
             mname, fname, w, h, arg);
    if (tiled == 0 || tiled < lin) {
        fprintf(stderr, "FAIL: %s (tiled=%zu lin=%zu)\n", label, tiled, lin);
        ++g_failures;
        return false;
    }

    /* PS Vita RAW is faithful to DeswizzlePSVitaRaw, which drops texels
     * whose mapped offset falls outside the image - that happens when a
     * dimension is not a multiple of 32, so the map is not invertible
     * there and a roundtrip cannot be expected to be the identity. */
    uint32_t fbw = 0, fbh = 0, fbb = 0;
    texc_block_dims(fmt, &fbw, &fbh, &fbb);
    if (mode == TEXC_SWIZZLE_PSVITA && fbw == 1 &&
        ((w % 32) || (h % 32))) {
        printf("  skip %s %s %ux%u (raw map is lossy off 32px grid)\n",
               mname, fname, w, h);
        return true;
    }

    std::vector<uint8_t> src(lin), mid(tiled, 0xEE), out(lin, 0xCD);
    for (size_t i = 0; i < lin; ++i)
        src[i] = (uint8_t)((i * 2654435761u) >> 13);

    int rc = texc::swizzle_convert(mode, fmt, w, h, src.data(), lin,
                                   mid.data(), tiled, arg, false);
    if (rc != TEXC_OK) {
        fprintf(stderr, "FAIL: %s %s %ux%u swizzle rc=%d\n", mname, fname, w, h, rc);
        ++g_failures;
        return false;
    }
    rc = texc::swizzle_convert(mode, fmt, w, h, mid.data(), tiled,
                               out.data(), lin, arg, true);
    if (rc != TEXC_OK) {
        fprintf(stderr, "FAIL: %s %s %ux%u unswizzle rc=%d\n", mname, fname, w, h, rc);
        ++g_failures;
        return false;
    }
    if (memcmp(src.data(), out.data(), lin) != 0) {
        size_t first = 0;
        while (first < lin && src[first] == out[first])
            ++first;
        fprintf(stderr, "FAIL: %s %s %ux%u roundtrip mismatch at byte %zu\n",
                mname, fname, w, h, first);
        ++g_failures;
        return false;
    }
    return true;
}

} /* anonymous namespace */

int main()
{
    static const struct { texc_swizzle_mode m; const char *n; uint32_t arg; } modes[] = {
        { TEXC_SWIZZLE_NONE,      "NONE",      0 },
        { TEXC_SWIZZLE_PS4,       "PS4",       0 },
        { TEXC_SWIZZLE_PS5,       "PS5",       0 },
        { TEXC_SWIZZLE_SWITCH,    "SWITCH",    0xFFFFFFFFu },
        { TEXC_SWIZZLE_PSVITA,    "PSVITA",    0 },
        { TEXC_SWIZZLE_X360,      "X360",      0 },
        { TEXC_SWIZZLE_PSP,       "PSP",       0 },
        { TEXC_SWIZZLE_3DS,       "3DS",       0 },
        { TEXC_SWIZZLE_WIIU,      "WIIU",      0 },
        { TEXC_SWIZZLE_DX12_64KB, "DX12_64KB", 0 },
    };
    static const struct { texc_format f; const char *n; } fmts[] = {
        { TEXC_FORMAT_BC1,   "BC1"   },
        { TEXC_FORMAT_BC3,   "BC3"   },
        { TEXC_FORMAT_BC7,   "BC7"   },
        { TEXC_FORMAT_RGBA8, "RGBA8" },
    };
    static const struct { uint32_t w, h; } sizes[] = {
        { 64, 64 }, { 256, 128 }, { 37, 23 }, { 512, 512 },
    };

    int total = 0, passed = 0;
    for (const auto &mo : modes)
        for (const auto &ft : fmts)
            for (const auto &sz : sizes) {
                ++total;
                if (roundtrip(mo.m, mo.n, ft.f, ft.n, sz.w, sz.h, mo.arg))
                    ++passed;
            }

    /* Explicit-arg variants. */
    ++total; if (roundtrip(TEXC_SWIZZLE_SWITCH, "SWITCH", TEXC_FORMAT_BC3, "BC3", 256, 128, 0)) ++passed;
    ++total; if (roundtrip(TEXC_SWIZZLE_SWITCH, "SWITCH", TEXC_FORMAT_BC3, "BC3", 256, 128, 3)) ++passed;
    ++total; if (roundtrip(TEXC_SWIZZLE_SWITCH, "SWITCH", TEXC_FORMAT_RGBA8, "RGBA8", 512, 512, 4)) ++passed;
    ++total; if (roundtrip(TEXC_SWIZZLE_X360,   "X360",   TEXC_FORMAT_RGBA8, "RGBA8", 256, 128, 8)) ++passed;
    ++total; if (roundtrip(TEXC_SWIZZLE_WIIU,   "WIIU",   TEXC_FORMAT_BC1,   "BC1",   256, 128, 0x500)) ++passed;

    /* Size sanity: padding behaviour. */
    {
        /* Switch tiled sizes are whole GOBs. */
        size_t s = texc::swizzled_size(TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC1,
                                       64, 64, 0xFFFFFFFFu);
        check(s != 0 && (s % 512) == 0, "SWITCH size is GOB aligned");
        check(s >= format_linear_size(TEXC_FORMAT_BC1, 64, 64), "SWITCH size >= linear");

        /* X360 pads to 32x32-block macro tiles: 64x64 BC1 = 16x16 blocks
         * -> 32x32 blocks * 8B. */
        s = texc::swizzled_size(TEXC_SWIZZLE_X360, TEXC_FORMAT_BC1, 64, 64, 0);
        check(s == 32 * 32 * 8, "X360 64x64 BC1 pads to 32x32 macro blocks");

        /* PS4 pads to 8x8 element tiles: 37x23 BC1 = 10x6 blocks -> 16x8. */
        s = texc::swizzled_size(TEXC_SWIZZLE_PS4, TEXC_FORMAT_BC1, 37, 23, 0);
        check(s == 16 * 8 * 8, "PS4 37x23 BC1 pads to 8x8 tiles");

        /* DX12 <= 64KB stays linear. */
        s = texc::swizzled_size(TEXC_SWIZZLE_DX12_64KB, TEXC_FORMAT_BC1, 256, 128, 0);
        check(s == format_linear_size(TEXC_FORMAT_BC1, 256, 128),
              "DX12 <=64KB not swizzled");
        s = texc::swizzled_size(TEXC_SWIZZLE_DX12_64KB, TEXC_FORMAT_BC1, 512, 512, 0);
        check(s == 2 * 65536, "DX12 512x512 BC1 = 2 x 64KB tiles");
    }

    /* Buffer bounds: short tiled buffer must be rejected, not overrun. */
    {
        const size_t lin = format_linear_size(TEXC_FORMAT_BC7, 64, 64);
        const size_t tiled = texc::swizzled_size(TEXC_SWIZZLE_PS4,
                                                 TEXC_FORMAT_BC7, 64, 64, 0);
        std::vector<uint8_t> a(lin, 1), b(tiled, 0);
        int rc = texc::swizzle_convert(TEXC_SWIZZLE_PS4, TEXC_FORMAT_BC7,
                                       64, 64, a.data(), lin, b.data(),
                                       tiled - 1, 0, false);
        check(rc == TEXC_ERR_BUFFER_TOO_SMALL, "short dst rejected");
        rc = texc::swizzle_convert(TEXC_SWIZZLE_PS4, TEXC_FORMAT_BC7,
                                   64, 64, b.data(), tiled - 1, a.data(),
                                   lin, 0, true);
        check(rc == TEXC_ERR_BUFFER_TOO_SMALL, "short src rejected");
    }

    printf("roundtrips: %d/%d passed, %d extra checks failed\n",
           passed, total, g_failures - (total - passed));
    if (g_failures == 0 && passed == total) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    return 1;
}

#endif /* TEXC_SELFTEST */
