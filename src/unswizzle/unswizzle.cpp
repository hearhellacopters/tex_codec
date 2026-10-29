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
 *   - PS5 legacy layout:     id-daemon RawTex Cooker (real layout: see
 *                            ps5_agc.cpp, after AMD addrlib)
 *   - Switch GOB sizing:     "Nintendo Switch Size Code" in ref/G1T.h
 *   - Switch 2 (Orin) GOB:   ScanMountGoat/tegra_swizzle (isOrin path)
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
#include "ps5_agc.h"

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

/* Copy n bytes applying a Xenos GPUENDIAN mode (0 none, 1 8in16, 2 8in32,
 * 3 16in32); n is a multiple of the swap unit. Each mode is its own
 * inverse. */
void copy_endian(uint8_t *d, const uint8_t *s, uint32_t n, uint32_t endian)
{
    switch (endian) {
    case 1:
        for (uint32_t i = 0; i < n; i += 2) {
            d[i]     = s[i + 1];
            d[i + 1] = s[i];
        }
        break;
    case 2:
        for (uint32_t i = 0; i < n; i += 4) {
            d[i]     = s[i + 3];
            d[i + 1] = s[i + 2];
            d[i + 2] = s[i + 1];
            d[i + 3] = s[i];
        }
        break;
    case 3:
        for (uint32_t i = 0; i < n; i += 4) {
            d[i]     = s[i + 2];
            d[i + 1] = s[i + 3];
            d[i + 2] = s[i];
            d[i + 3] = s[i + 1];
        }
        break;
    default:
        memcpy(d, s, n);
        break;
    }
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

/* Padded pitch in elements. 3DS: 8-aligned. PS4 (GNM Thin_1dThin, checked
 * against Sony's GpuAddress with tools/ps4_oracle): 8-aligned, then grown
 * by 8 until one slice (pitch * align8(height) * bytes) is a multiple of
 * 256 bytes - which only changes anything for 1- and 2-byte elements. */
uint32_t morton8x8_pitch(texc_swizzle_mode mode, const Grid &g)
{
    uint32_t pw = align_up(g.ew, 8);
    if (mode == TEXC_SWIZZLE_PS4) {
        const uint64_t ph = align_up(g.eh, 8);
        while (((uint64_t)pw * ph * g.eb) % 256)
            pw += 8;
    }
    return pw;
}

/* PS4 Thin_1dThin pitch for a cols x rows element mip. */
uint32_t ps4_pitch(const Grid &g, uint32_t cols, uint32_t rows)
{
    Grid sg = g;
    sg.ew = cols;
    sg.eh = rows;
    return morton8x8_pitch(TEXC_SWIZZLE_PS4, sg);
}

/* Element grid GNM pads a PS4 mip to before tiling. A lone mip keeps its
 * size. In a chain of several mips (any size), each mip's width is mip 0's
 * width rounded up to a power of two, then halved per level, and its height
 * (and a volume's depth) is the mip's own, rounded up to a power of two -
 * fitted against KTGL / GpuAddress on 1,000 random 2D and volume chains
 * (tools/ps4_oracle). */
void ps4_mip_dims(const Grid &g, uint32_t w, uint32_t h, uint32_t m,
                  uint32_t mips, uint32_t *cols, uint32_t *rows)
{
    uint32_t mw = (w >> m) ? (w >> m) : 1;
    uint32_t mh = (h >> m) ? (h >> m) : 1;
    if (mips > 1) {
        mw = (next_pow2(w) >> m) ? (next_pow2(w) >> m) : 1;
        mh = next_pow2(mh);
    }
    *cols = (mw + g.bw - 1) / g.bw;
    *rows = (mh + g.bh - 1) / g.bh;
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

/* ------------------------------------------- Switch (Tegra X1) GOBs ----- */
/* 64-byte x 8-row GOBs stacked into blocks of `bh` GOBs. Matches
 * swizzled_mip_size() in ref/G1T.h and the layout consumed by
 * Image_UntileBlockLinearGOBs. xb = byte offset within the row of blocks. */
/* Byte offset of (xb, y) inside one 512-byte GOB (64 bytes x 8 rows).
 * Tegra X1:  bits 0-3 x0-3, 4 y0, 5 x4, 6 y1, 7 y2, 8 x5.
 * Orin (Switch 2) swaps bits 5 and 6: 4 y0, 5 y1, 6 x4, 7 y2 - from the
 * isOrin path of tegra_swizzle's deswizzle_inner (ScanMountGoat). */
inline uint32_t gob_offset(uint32_t xb, uint32_t y, bool orin)
{
    if (orin)
        return ((xb % 64) / 32) * 256 + ((y % 8) / 4) * 128 +
               ((xb % 32) / 16) * 64 + (y % 4) * 16 + (xb % 16);
    return ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
           ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
}

inline uint64_t map_switch_gob(uint32_t width_gobs, uint32_t bh,
                               uint32_t xb, uint32_t y, bool orin)
{
    uint64_t a = (uint64_t)(y / (8 * bh)) * 512ull * bh * width_gobs +
                 (uint64_t)(xb / 64) * 512ull * bh +
                 (uint64_t)((y % (8 * bh)) / 8) * 512;
    return a + gob_offset(xb, y, orin);
}

/* Port of block_height_enum() / mip_block_height() from ref/G1T.h (the
 * same rules as tegra_swizzle's block_height_mip0 / mip_block_height). */
uint32_t switch_block_height_mip0(uint32_t blocks_y)
{
    const uint32_t h = blocks_y + blocks_y / 2;
    if (h >= 128) return 16;
    if (h >= 64)  return 8;
    if (h >= 32)  return 4;
    if (h >= 16)  return 2;
    return 1;
}

uint32_t switch_mip_block_height(uint32_t blocks_y, uint32_t bh)
{
    while (blocks_y <= (bh / 2) * 8 && bh > 1)
        bh /= 2;
    return bh;
}

uint32_t switch_auto_block_height(uint32_t blocks_y)
{
    return switch_mip_block_height(blocks_y, switch_block_height_mip0(blocks_y));
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
    if (arg & ~(TEXC_X360_PITCH_MASK | TEXC_X360_ENDIAN_MASK |
                TEXC_X360_LOADER_ROWS))
        return false;
    const uint32_t ov = arg & TEXC_X360_PITCH_MASK;
    uint32_t p = ov ? ov : g.eb;
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

/* Xenos byte order for `arg` (see texc_x360_endian): an explicit mode, or
 * the one KTGL's X360 loader declares for the element size. Returns the
 * GPUENDIAN value, or -1 when the swap unit does not divide the pitch. */
int x360_endian(const Grid &g, uint32_t arg, uint32_t p)
{
    uint32_t e = (arg & TEXC_X360_ENDIAN_MASK) >> 8;
    if (e)
        --e;
    else if (g.bw > 1 || g.bh > 1)
        e = 1;                           /* DXT: 8in16 */
    else
        e = g.eb == 1 ? 0 : (g.eb == 2 || g.eb == 8) ? 1 : 2;
    if (e > 3)
        return -1;
    const uint32_t unit = e == 0 ? 1 : e == 1 ? 2 : 4;
    return p % unit ? -1 : (int)e;
}

/* Tiled X360 surface size: 32x32-element macro tiles, the whole level
 * then aligned to 4 KB (Xenia's guest layout). The 4 KB alignment only
 * matters for 1- and 2-byte elements, whose address function spreads a
 * 32-row band of macro tiles over whole 4 KB pages. */
uint64_t x360_tiled_size(uint32_t gw, uint32_t gh, uint32_t p)
{
    const uint64_t raw = (uint64_t)align_up(gw, 32) * align_up(gh, 32) * p;
    return (raw + 4095) / 4096 * 4096;
}

int convert_x360(const Grid &g, uint32_t arg,
                 const uint8_t *src, size_t src_size,
                 uint8_t *dst, size_t dst_size, bool to_linear)
{
    uint32_t p = 0, gw = 0, gh = 0;
    if (!x360_texel_grid(g, arg, &p, &gw, &gh))
        return TEXC_ERR_INVALID_ARG;
    const int endian = x360_endian(g, arg, p);
    if (endian < 0)
        return TEXC_ERR_INVALID_ARG;

    const uint32_t pw = align_up(gw, 32);
    const uint64_t tiled_size = x360_tiled_size(gw, gh, p);
    const uint64_t lin_size = (uint64_t)gw * gh * p;
    if (tiled_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;

    /* LOADER_ROWS: the tiled side is the surface as the loader fills it -
     * stored row r (row bytes) lands at surface offset r * pitch, and
     * everything else in the surface is never written (zeros). */
    const bool rows = (arg & TEXC_X360_LOADER_ROWS) != 0;
    const uint64_t row = (uint64_t)gw * p, pitch = (uint64_t)pw * p;
    const uint64_t stored_size = rows ? lin_size : tiled_size;

    if (to_linear) {
        if ((!rows && src_size < tiled_size) || dst_size < lin_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
    } else {
        if (src_size < lin_size || dst_size < stored_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memset(dst, 0, (size_t)stored_size);
    }

    const uint64_t total = tiled_size / p;
    for (uint64_t t = 0; t < total; ++t) {
        const uint32_t x = xg_tiled_x((uint32_t)t, pw, p);
        const uint32_t y = xg_tiled_y((uint32_t)t, pw, p);
        if (x >= gw || y >= gh)
            continue; /* padding element */
        uint64_t toff = t * p;
        const uint64_t loff = ((uint64_t)y * gw + x) * p;
        if (rows) {
            const uint64_t r = toff / pitch, c = toff % pitch;
            const bool stored = r < gh && c < row &&
                                (!to_linear || r * row + c + p <= src_size);
            if (!stored) {
                /* never loaded: reads as zeros, cannot be written */
                if (to_linear)
                    memset(dst + (size_t)loff, 0, p);
                continue;
            }
            toff = r * row + c;
        }
        /* Xenos data is big-endian per the format's GPUENDIAN mode; every
         * mode is its own inverse. */
        if (to_linear)
            copy_endian(dst + (size_t)loff, src + (size_t)toff, p,
                        (uint32_t)endian);
        else
            copy_endian(dst + (size_t)toff, src + (size_t)loff, p,
                        (uint32_t)endian);
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
/* The real PS5 layout (AMD GFX10 standard swizzle, block raster + mip
 * tail) lives in ps5_agc.cpp. What follows is the pre-1.6 layout, kept
 * reachable as TEXC_PS5_TILE_LEGACY: a port of UnswizzlePS5 / PS5morton
 * (source id-daemon RawTex Cooker). The tiled data is a sequential stream:
 * 128x128-element macro tiles for raw formats, 64x64-block macro tiles for
 * block formats, each filled in the nested morton/sub-tile order below.
 * For 16-byte and 4-byte elements this happens to equal STANDARD_64KB. */
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

uint64_t ps5_legacy_tiled_size(const Grid &g)
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

int convert_ps5_legacy(const Grid &g,
                       const uint8_t *src, size_t src_size,
                       uint8_t *dst, size_t dst_size, bool to_linear)
{
    const uint64_t tiled_size = ps5_legacy_tiled_size(g);
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

/* ------------------------------------------------ D3D12 64KB tiles ------ */
/* KTGL's on-disk form of a D3D12 64KB-tiled (reserved) texture, from the
 * CPU upload path of Rise of the Ronin PC (sub_141E3CD10): each mip is cut
 * into tiles of the D3D12 standard 64KB shape for its element size, tiles
 * go row-major, and each tile is stored contiguously as its own rows. Edge
 * tiles are only as wide / tall as what is left, so the stored size is
 * exactly the linear size, and a mip at most one tile wide is plain
 * linear. Shapes are in elements (blocks for BCn): BC1/BC4 tiles are
 * 512x256 pixels, BC2/3/5/6H/7 256x256. */
bool dx12_tile_shape(uint32_t eb, uint32_t *tw, uint32_t *th)
{
    switch (eb) {
    case 1:  *tw = 256; *th = 256; return true;
    case 2:  *tw = 256; *th = 128; return true;
    case 4:  *tw = 128; *th = 128; return true;
    case 8:  *tw = 128; *th = 64;  return true;
    case 16: *tw = 64;  *th = 64;  return true;
    default: return false;
    }
}

int convert_dx12_64kb(const Grid &g,
                      const uint8_t *src, size_t src_size,
                      uint8_t *dst, size_t dst_size, bool to_linear)
{
    uint32_t tw = 0, th = 0;
    if (!dx12_tile_shape(g.eb, &tw, &th))
        return TEXC_ERR_UNSUPPORTED;
    const uint64_t row = (uint64_t)g.ew * g.eb;
    const uint64_t size = row * g.eh;
    if (size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;
    if (src_size < size || dst_size < size)
        return TEXC_ERR_BUFFER_TOO_SMALL;

    uint64_t toff = 0;
    for (uint32_t y0 = 0; y0 < g.eh; y0 += th) {
        const uint32_t ch = umin32(th, g.eh - y0);
        for (uint32_t x0 = 0; x0 < g.ew; x0 += tw) {
            const uint32_t n = umin32(tw, g.ew - x0) * g.eb;
            for (uint32_t r = 0; r < ch; ++r, toff += n) {
                const uint64_t loff = (uint64_t)(y0 + r) * row +
                                      (uint64_t)x0 * g.eb;
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
    if (g.bw == 1 && g.bh == 1) {
        /* raw texels: a GX2 format of the same element size (R8, R8G8,
         * R8G8B8A8, R16G16B16A16, R32G32B32A32) - addrlib only uses its
         * bits per element */
        switch (g.eb) {
        case 1:  *hw = 0x01; return true;
        case 2:  *hw = 0x07; return true;
        case 4:  *hw = 0x1A; return true;
        case 8:  *hw = 0x1F; return true;
        case 16: *hw = 0x23; return true;
        }
    }
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
                           x, y, 0, bpp, pitch, sheight, tile_mode,
                           pipe_swz, bank_swz);
                   });
}

/* Whole GX2 surface, as KTGL's Wii U loader hands it to GX2 (tile mode
 * DEFAULT = 2D_TILED_THIN1, Hyrule Warriors): mip 0 at offset 0, then
 * each further level at its addrlib base alignment. Levels narrower than
 * a macro tile resolve to 1D_TILED_THIN1, padded to 8x8 micro tiles -
 * the mips the G1T payload sizes and the decoded pixels both confirm.
 * GX2 keeps every slice of a level together, so several slices are only
 * expressible here (slice-major) for a single mip. */
int layout_wiiu(const Grid &g, uint32_t w, uint32_t h, uint32_t mips,
                uint32_t slices, texc_surface_layout *out)
{
    uint32_t hw = 0;
    if (!wiiu_hw_format(g, &hw))
        return TEXC_ERR_UNSUPPORTED;
    if (mips > TEXC_MAX_MIPS)
        return TEXC_ERR_INVALID_ARG;
    if (mips > 1 && slices > 1)
        return TEXC_ERR_UNSUPPORTED;
    memset(out, 0, sizeof *out);
    out->mip_count = mips;
    out->slice_count = slices;
    out->first_mip_in_tail = mips;
    out->block_width = out->block_height = 8;   /* micro tile */
    out->block_bytes = 64 * g.eb;
    out->element_bytes = g.eb;
    uint64_t off = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        gx2::SurfaceInfo si;
        if (!gx2::compute_surface_info(hw, w, h, slices, 4, m, &si) ||
            si.bpp / 8 != g.eb)
            return TEXC_ERR_UNSUPPORTED;
        texc_mip_layout &mi = out->mips[m];
        const uint32_t mw = (w >> m) ? (w >> m) : 1;
        const uint32_t mh = (h >> m) ? (h >> m) : 1;
        mi.width = (mw + g.bw - 1) / g.bw;
        mi.height = (mh + g.bh - 1) / g.bh;
        mi.padded_width = si.pitch;
        mi.padded_height = si.height;
        off = (off + si.base_align - 1) / si.base_align * si.base_align;
        mi.offset = off;
        mi.size = si.surf_size / slices;
        off += si.surf_size;
    }
    out->slice_size = slices > 1 ? out->mips[0].size : off;
    out->total_size = off;
    return TEXC_OK;
}

/* Whole PS4 surface as KTGL stores it (Thin_1dThin for tiled formats):
 * mips back to back, mip 0 first, each padded as ps4_mip_dims says and
 * tiled in 8x8 Morton micro-tiles at its own pitch. Several slices are
 * supported for a single mip (slices back to back); with a mip chain the
 * slice order is not pinned down yet. */
int layout_ps4(const Grid &g, uint32_t w, uint32_t h, uint32_t mips,
               uint32_t slices, texc_surface_layout *out)
{
    if (mips > TEXC_MAX_MIPS)
        return TEXC_ERR_INVALID_ARG;
    if (mips > 1 && slices > 1)
        return TEXC_ERR_UNSUPPORTED;
    memset(out, 0, sizeof *out);
    out->mip_count = mips;
    out->slice_count = slices;
    out->first_mip_in_tail = mips;
    out->block_width = out->block_height = 8;
    out->block_bytes = 64 * g.eb;
    out->element_bytes = g.eb;
    uint64_t off = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t pc = 0, pr = 0;
        ps4_mip_dims(g, w, h, m, mips, &pc, &pr);
        texc_mip_layout &mi = out->mips[m];
        const uint32_t mw = (w >> m) ? (w >> m) : 1;
        const uint32_t mh = (h >> m) ? (h >> m) : 1;
        mi.width = (mw + g.bw - 1) / g.bw;
        mi.height = (mh + g.bh - 1) / g.bh;
        mi.padded_width = ps4_pitch(g, pc, pr);
        mi.padded_height = align_up(pr, 8);
        mi.offset = off;
        mi.size = (uint64_t)mi.padded_width * mi.padded_height * g.eb;
        off += mi.size;
    }
    out->slice_size = off;
    out->total_size = off * slices;
    return TEXC_OK;
}

int convert_mip_ps4(const Grid &g, const texc_surface_layout &lay,
                    uint32_t mip, uint32_t slice,
                    const uint8_t *src, size_t src_size,
                    uint8_t *dst, size_t dst_size, bool to_linear)
{
    const texc_mip_layout &mi = lay.mips[mip];
    const uint64_t lin = (uint64_t)mi.width * mi.height * g.eb;
    if (lay.total_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;
    if (to_linear ? (src_size < lay.total_size || dst_size < lin)
                  : (src_size < lin || dst_size < lay.total_size))
        return TEXC_ERR_BUFFER_TOO_SMALL;
    const uint64_t base = (uint64_t)slice * lay.slice_size + mi.offset;
    for (uint32_t y = 0; y < mi.height; ++y)
        for (uint32_t x = 0; x < mi.width; ++x) {
            const uint64_t toff =
                base + map_morton8x8(mi.padded_width, x, y, g.eb);
            const uint64_t loff = ((uint64_t)y * mi.width + x) * g.eb;
            if (to_linear)
                memcpy(dst + (size_t)loff, src + (size_t)toff, g.eb);
            else
                memcpy(dst + (size_t)toff, src + (size_t)loff, g.eb);
        }
    return TEXC_OK;
}

/* Whole Switch / Switch 2 surface: NVN's block-linear storage, which is
 * what KTGL reads a tiled G1T payload into (nvnTextureGetStorageSize bytes,
 * Age of Calamity) and what tegra_swizzle's surface functions compute.
 * Each layer holds its mips back to back, each mip in GOB blocks of its
 * own height (mip 0's, halved while the mip still fits in half of it);
 * with several layers every layer is padded to a whole block of GOBs,
 * sized from mip 0's block height halved while half of it still covers
 * the image height in PIXELS (tegra_swizzle's rule). */
struct SwitchMip {
    uint32_t cols, rows, bh, width_gobs;
    uint64_t size;
};

bool switch_mip(const Grid &g, uint32_t w, uint32_t h, uint32_t bh0,
                uint32_t m, SwitchMip *out)
{
    const uint32_t mw = (w >> m) ? (w >> m) : 1;
    const uint32_t mh = (h >> m) ? (h >> m) : 1;
    out->cols = (mw + g.bw - 1) / g.bw;
    out->rows = (mh + g.bh - 1) / g.bh;
    out->bh = switch_mip_block_height(out->rows, bh0);
    const uint64_t wg = ((uint64_t)out->cols * g.eb + 63) / 64;
    if (wg > 0xFFFFFFFFu)
        return false;
    out->width_gobs = (uint32_t)wg;
    out->size = wg * ((out->rows + out->bh * 8 - 1) / (out->bh * 8)) *
                out->bh * 512ull;
    return true;
}

bool switch_bh0(const Grid &g, uint32_t arg, uint32_t *bh0)
{
    if (arg == 0xFFFFFFFFu) {
        *bh0 = switch_block_height_mip0(g.eh);
        return true;
    }
    if (arg > 5)
        return false;
    *bh0 = 1u << arg;
    return true;
}

int layout_switch(const Grid &g, uint32_t w, uint32_t h, uint32_t mips,
                  uint32_t slices, uint32_t arg, texc_surface_layout *out)
{
    uint32_t bh0 = 0;
    if (mips > TEXC_MAX_MIPS || !switch_bh0(g, arg, &bh0))
        return TEXC_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->mip_count = mips;
    out->slice_count = slices;
    out->first_mip_in_tail = mips;
    out->block_width = g.eb <= 64 ? 64 / g.eb : 1;  /* one GOB wide */
    out->block_height = 8 * bh0;
    out->block_bytes = 512 * bh0;
    out->element_bytes = g.eb;
    uint64_t off = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        SwitchMip sm;
        if (!switch_mip(g, w, h, bh0, m, &sm))
            return TEXC_ERR_INVALID_ARG;
        texc_mip_layout &mi = out->mips[m];
        mi.width = sm.cols;
        mi.height = sm.rows;
        mi.padded_width = (uint32_t)((uint64_t)sm.width_gobs * 64 / g.eb);
        mi.padded_height = (sm.rows + sm.bh * 8 - 1) / (sm.bh * 8) * sm.bh * 8;
        mi.offset = off;
        mi.size = sm.size;
        off += sm.size;
    }
    if (slices > 1) {
        uint32_t bh = bh0;
        while (bh >= 2 && 8 * (bh / 2) >= h)
            bh /= 2;
        const uint64_t a = 512ull * bh;
        off = (off + a - 1) / a * a;
    }
    out->slice_size = off;
    out->total_size = off * slices;
    return TEXC_OK;
}

int convert_mip_switch(const Grid &g, uint32_t w, uint32_t h,
                       const texc_surface_layout &lay, uint32_t arg,
                       uint32_t mip, uint32_t slice, bool orin,
                       const uint8_t *src, size_t src_size,
                       uint8_t *dst, size_t dst_size, bool to_linear)
{
    uint32_t bh0 = 0;
    SwitchMip sm;
    if (!switch_bh0(g, arg, &bh0) || !switch_mip(g, w, h, bh0, mip, &sm))
        return TEXC_ERR_INVALID_ARG;
    const uint64_t lin = (uint64_t)sm.cols * sm.rows * g.eb;
    if (lay.total_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;
    if (to_linear ? (src_size < lay.total_size || dst_size < lin)
                  : (src_size < lin || dst_size < lay.total_size))
        return TEXC_ERR_BUFFER_TOO_SMALL;
    const uint64_t base = (uint64_t)slice * lay.slice_size + lay.mips[mip].offset;
    for (uint32_t y = 0; y < sm.rows; ++y)
        for (uint32_t x = 0; x < sm.cols; ++x) {
            const uint64_t toff = base + map_switch_gob(sm.width_gobs, sm.bh,
                                                        x * g.eb, y, orin);
            const uint64_t loff = ((uint64_t)y * sm.cols + x) * g.eb;
            if (toff + g.eb > lay.total_size)
                return TEXC_ERR_BUFFER_TOO_SMALL;
            if (to_linear)
                memcpy(dst + (size_t)loff, src + (size_t)toff, g.eb);
            else
                memcpy(dst + (size_t)toff, src + (size_t)loff, g.eb);
        }
    return TEXC_OK;
}

int convert_mip_wiiu(const Grid &g, uint32_t w, uint32_t h,
                     const texc_surface_layout &lay, uint32_t arg,
                     uint32_t mip, uint32_t slice,
                     const uint8_t *src, size_t src_size,
                     uint8_t *dst, size_t dst_size, bool to_linear)
{
    uint32_t hw = 0;
    gx2::SurfaceInfo si;
    if (!wiiu_hw_format(g, &hw) ||
        !gx2::compute_surface_info(hw, w, h, lay.slice_count, 4, mip, &si))
        return TEXC_ERR_UNSUPPORTED;
    const texc_mip_layout &mi = lay.mips[mip];
    const uint64_t lin = (uint64_t)mi.width * mi.height * g.eb;
    if (lay.total_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;
    if (to_linear ? (src_size < lay.total_size || dst_size < lin)
                  : (src_size < lin || dst_size < lay.total_size))
        return TEXC_ERR_BUFFER_TOO_SMALL;

    const uint32_t pipe_swz = (arg >> 8) & 1;
    const uint32_t bank_swz = (arg >> 9) & 3;
    for (uint32_t y = 0; y < mi.height; ++y)
        for (uint32_t x = 0; x < mi.width; ++x) {
            const uint64_t toff = mi.offset + (uint64_t)gx2::addr_from_coord(
                x, y, slice, si.bpp, si.pitch, si.height, si.tile_mode,
                pipe_swz, bank_swz);
            const uint64_t loff = ((uint64_t)y * mi.width + x) * g.eb;
            if (toff + g.eb > lay.total_size)
                return TEXC_ERR_BUFFER_TOO_SMALL;
            if (to_linear)
                memcpy(dst + (size_t)loff, src + (size_t)toff, g.eb);
            else
                memcpy(dst + (size_t)toff, src + (size_t)loff, g.eb);
        }
    return TEXC_OK;
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
        return (uint64_t)morton8x8_pitch(mode, g) * align_up(g.eh, 8) * g.eb;

    case TEXC_SWIZZLE_PS5: {
        if (arg == TEXC_PS5_TILE_LEGACY)
            return ps5_legacy_tiled_size(g);
        texc_surface_layout lay;
        if (ps5::surface_layout(w, h, g.bw, g.bh, g.eb, 1, 1, arg, &lay) !=
            TEXC_OK)
            return 0;
        return lay.total_size;
    }

    case TEXC_SWIZZLE_SWITCH:
    case TEXC_SWIZZLE_SWITCH2: {
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
        if (!x360_texel_grid(g, arg, &p, &gw, &gh) ||
            x360_endian(g, arg, p) < 0)
            return 0;
        if (arg & TEXC_X360_LOADER_ROWS)
            return (uint64_t)gw * gh * p;
        return x360_tiled_size(gw, gh, p);
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
        /* Edge tiles are stored partial: always the linear size. */
        uint32_t tw = 0, th = 0;
        return dx12_tile_shape(g.eb, &tw, &th) ? lin : 0;
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
        const uint32_t pw = morton8x8_pitch(mode, g);
        return run_map(g, ts, src, src_size, dst, dst_size, dir_to_linear,
                       [&](uint32_t x, uint32_t y) {
                           return map_morton8x8(pw, x, y, g.eb);
                       });
    }

    case TEXC_SWIZZLE_PS5: {
        if (arg == TEXC_PS5_TILE_LEGACY)
            return convert_ps5_legacy(g, src, src_size, dst, dst_size,
                                      dir_to_linear);
        texc_surface_layout lay;
        const int rc = ps5::surface_layout(width, height, g.bw, g.bh, g.eb,
                                           1, 1, arg, &lay);
        if (rc != TEXC_OK)
            return rc;
        return ps5::convert_mip(lay, arg, 0, 0, src, src_size, dst, dst_size,
                                dir_to_linear, true);
    }

    case TEXC_SWIZZLE_SWITCH:
    case TEXC_SWIZZLE_SWITCH2: {
        uint32_t bh = 0;
        if (!switch_block_height(g.eh, arg, &bh))
            return TEXC_ERR_INVALID_ARG;
        const uint64_t ts = tiled_size_impl(mode, g, width, height, arg);
        const uint32_t width_gobs =
            (uint32_t)(((uint64_t)g.ew * g.eb + 63) / 64);
        const bool orin = mode == TEXC_SWIZZLE_SWITCH2;
        return run_map(g, ts, src, src_size, dst, dst_size, dir_to_linear,
                       [&](uint32_t x, uint32_t y) {
                           return map_switch_gob(width_gobs, bh, x * g.eb, y,
                                                 orin);
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

    case TEXC_SWIZZLE_DX12_64KB:
        return convert_dx12_64kb(g, src, src_size, dst, dst_size,
                                 dir_to_linear);

    default:
        return TEXC_ERR_UNSUPPORTED;
    }
}

/* ------------------------------------------------- mip-chain surfaces --- */

namespace {

/* TEXC_SWIZZLE_NONE: mips back to back, mip 0 first, no padding. */
int layout_linear(const Grid &g, uint32_t w, uint32_t h,
                  uint32_t mips, uint32_t slices, texc_surface_layout *out)
{
    if (mips > TEXC_MAX_MIPS)
        return TEXC_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->mip_count = mips;
    out->slice_count = slices;
    out->first_mip_in_tail = mips;
    out->block_width = out->block_height = 1;
    out->block_bytes = out->element_bytes = g.eb;
    uint64_t off = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        texc_mip_layout &mi = out->mips[m];
        const uint32_t mw = (w >> m) ? (w >> m) : 1;
        const uint32_t mh = (h >> m) ? (h >> m) : 1;
        mi.width = mi.padded_width = (mw + g.bw - 1) / g.bw;
        mi.height = mi.padded_height = (mh + g.bh - 1) / g.bh;
        mi.offset = off;
        mi.size = (uint64_t)mi.width * mi.height * g.eb;
        off += mi.size;
    }
    out->slice_size = off;
    out->total_size = off * slices;
    return TEXC_OK;
}

} /* namespace */

int surface_layout(texc_swizzle_mode mode, texc_format fmt,
                   uint32_t width, uint32_t height,
                   uint32_t mips, uint32_t slices, uint32_t arg,
                   texc_surface_layout *out)
{
    Grid g;
    if (!out || !get_grid(fmt, width, height, &g) || !mips || !slices)
        return TEXC_ERR_INVALID_ARG;
    int rc;
    switch (mode) {
    case TEXC_SWIZZLE_NONE:
        rc = layout_linear(g, width, height, mips, slices, out);
        break;
    case TEXC_SWIZZLE_PS5:
        if (arg == TEXC_PS5_TILE_LEGACY)
            return TEXC_ERR_UNSUPPORTED;
        rc = ps5::surface_layout(width, height, g.bw, g.bh, g.eb,
                                 mips, slices, arg, out);
        break;
    case TEXC_SWIZZLE_WIIU:
        rc = layout_wiiu(g, width, height, mips, slices, out);
        break;
    case TEXC_SWIZZLE_SWITCH:
    case TEXC_SWIZZLE_SWITCH2:
        rc = layout_switch(g, width, height, mips, slices, arg, out);
        break;
    case TEXC_SWIZZLE_PS4:
        rc = layout_ps4(g, width, height, mips, slices, out);
        break;
    default:
        return TEXC_ERR_UNSUPPORTED;
    }
    if (rc == TEXC_OK)
        for (uint32_t m = 0; m < out->mip_count; ++m)
            out->mips[m].depth = 1;
    return rc;
}

/* ---------------------------------------------------- volume surfaces --- */

namespace {

/* tegra_swizzle's block depth for a mip `depth` slices deep. */
uint32_t switch_block_depth(uint32_t depth)
{
    const uint32_t d = depth + depth / 2;
    if (d >= 16) return 16;
    if (d >= 8)  return 8;
    if (d >= 4)  return 4;
    if (d >= 2)  return 2;
    return 1;
}

struct VolumeMip {
    uint32_t cols, rows, depth;
    uint32_t bh, bd, width_gobs;    /* Switch only */
    uint64_t slab;                  /* Switch: bytes per bd slices */
    uint32_t pw, ph;                /* PS4: padded pitch / height */
    bool thick;                     /* PS4: Thick_1dThick, else per-slice
                                     * Thin_1dThin */
    uint64_t size;
};

/* GNM Thick_1dThick: 8x8x4-element micro tiles, row by row, then a 4-slice
 * slab at a time. Element order inside a micro tile, lowest bit first, as
 * Sony's GpuAddress tileSurface lays it out (tools/ps4_oracle):
 *   1 / 2-byte texels x0 y0 x1 y1 z0 z1 x2 y2
 *   4-byte texels     x0 y0 x1 z0 y1 z1 x2 y2
 *   8-byte texels     x0 y0 z0 x1 y1 z1 x2 y2
 * (block-compressed volumes come out Thin_1dThin a slice at a time) */
inline uint32_t ps4_thick_index(const Grid &g, uint32_t x, uint32_t y,
                                uint32_t z)
{
    const uint32_t x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1;
    const uint32_t y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
    const uint32_t z0 = z & 1, z1 = (z >> 1) & 1;
    if (g.eb <= 2)
        return x0 | y0 << 1 | x1 << 2 | y1 << 3 | z0 << 4 | z1 << 5 |
               x2 << 6 | y2 << 7;
    if (g.eb == 4)
        return x0 | y0 << 1 | x1 << 2 | z0 << 3 | y1 << 4 | z1 << 5 |
               x2 << 6 | y2 << 7;
    return x0 | y0 << 1 | z0 << 2 | x1 << 3 | y1 << 4 | z1 << 5 |
           x2 << 6 | y2 << 7;
}

bool volume_mip(texc_swizzle_mode mode, const Grid &g, uint32_t w,
                uint32_t h, uint32_t d, uint32_t m, uint32_t mips,
                VolumeMip *out)
{
    const uint32_t mw = (w >> m) ? (w >> m) : 1;
    const uint32_t mh = (h >> m) ? (h >> m) : 1;
    out->depth = (d >> m) ? (d >> m) : 1;
    out->cols = (mw + g.bw - 1) / g.bw;
    out->rows = (mh + g.bh - 1) / g.bh;
    if (mode == TEXC_SWIZZLE_NONE) {
        out->size = (uint64_t)out->cols * out->rows * out->depth * g.eb;
        return true;
    }
    if (mode == TEXC_SWIZZLE_PS4) {
        /* KTGL's volume setup (eboot 0x22B8010) asks GNM for Thick_1dThick
         * for every tiled format but 16-byte raw texels (RGBA32F). GNM
         * lays out raw texels thick; block-compressed formats and RGBA32F
         * come out Thin_1dThin a slice at a time, slices not padded.
         * Sizes and bytes as GpuAddress produces them (tools/ps4_oracle) */
        out->thick = g.bw == 1 && g.bh == 1 && g.eb != 16;
        uint32_t pc = 0, pr = 0, pd = out->depth;
        ps4_mip_dims(g, w, h, m, mips, &pc, &pr);
        if (mips > 1)
            pd = next_pow2(pd);
        out->ph = align_up(pr, 8);
        if (out->thick) {
            out->pw = align_up(pc, 8);
            out->size = (uint64_t)out->pw * out->ph * align_up(pd, 4) * g.eb;
        } else {
            out->pw = ps4_pitch(g, pc, pr);
            out->size = (uint64_t)out->pw * out->ph * g.eb * pd;
        }
        return true;
    }
    /* NVN gives a volume a block height of one GOB (tegra_swizzle forces
     * block_height_mip0 to 1 for depth > 1); depth 1 keeps the 2D rule */
    const uint32_t bh0 = d > 1 ? 1 : switch_block_height_mip0(g.eh);
    out->bh = switch_mip_block_height(out->rows, bh0);
    out->bd = switch_block_depth(out->depth);
    const uint64_t wg = ((uint64_t)out->cols * g.eb + 63) / 64;
    if (wg > 0xFFFFFFFFu)
        return false;
    out->width_gobs = (uint32_t)wg;
    out->slab = wg * ((out->rows + out->bh * 8 - 1) / (out->bh * 8)) *
                512ull * out->bh * out->bd;
    out->size = (uint64_t)((out->depth + out->bd - 1) / out->bd) * out->slab;
    return true;
}

/* Byte offset of (xb, y, z) in a Switch 3D block-linear mip: blocks of
 * 1 x bh x bd GOBs, across x, then down y, then deeper in z. */
inline uint64_t map_switch_gob_3d(const VolumeMip &v, uint32_t xb, uint32_t y,
                                  uint32_t z, bool orin)
{
    const uint64_t block = 512ull * v.bh * v.bd;
    return (uint64_t)(z / v.bd) * v.slab +
           (uint64_t)(z % v.bd) * 512ull * v.bh +
           (uint64_t)(y / (8 * v.bh)) * block * v.width_gobs +
           (uint64_t)((y % (8 * v.bh)) / 8) * 512 +
           (uint64_t)(xb / 64) * block + gob_offset(xb, y, orin);
}

bool volume_mode(texc_swizzle_mode mode)
{
    return mode == TEXC_SWIZZLE_NONE || mode == TEXC_SWIZZLE_PS4 ||
           mode == TEXC_SWIZZLE_SWITCH || mode == TEXC_SWIZZLE_SWITCH2;
}

} /* namespace */

int volume_layout(texc_swizzle_mode mode, texc_format fmt,
                  uint32_t width, uint32_t height, uint32_t depth,
                  uint32_t mips, uint32_t arg, texc_surface_layout *out)
{
    Grid g;
    if (!out || !get_grid(fmt, width, height, &g) || !depth || !mips ||
        mips > TEXC_MAX_MIPS)
        return TEXC_ERR_INVALID_ARG;
    if (!volume_mode(mode))
        return TEXC_ERR_UNSUPPORTED;
    if ((mode == TEXC_SWIZZLE_SWITCH || mode == TEXC_SWIZZLE_SWITCH2) &&
        arg != 0xFFFFFFFFu && arg > 5)
        return TEXC_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->mip_count = mips;
    out->slice_count = 1;
    out->first_mip_in_tail = mips;
    out->element_bytes = g.eb;
    uint64_t off = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        VolumeMip v;
        if (!volume_mip(mode, g, width, height, depth, m, mips, &v))
            return TEXC_ERR_INVALID_ARG;
        texc_mip_layout &mi = out->mips[m];
        mi.width = v.cols;
        mi.height = v.rows;
        mi.depth = v.depth;
        if (mode == TEXC_SWIZZLE_NONE) {
            mi.padded_width = v.cols;
            mi.padded_height = v.rows;
        } else if (mode == TEXC_SWIZZLE_PS4) {
            mi.padded_width = v.pw;
            mi.padded_height = v.ph;
        } else {
            mi.padded_width = (uint32_t)((uint64_t)v.width_gobs * 64 / g.eb);
            mi.padded_height = (v.rows + v.bh * 8 - 1) / (v.bh * 8) * v.bh * 8;
        }
        if (m == 0) {
            if (mode == TEXC_SWIZZLE_NONE) {
                out->block_width = out->block_height = 1;
                out->block_bytes = g.eb;
            } else if (mode == TEXC_SWIZZLE_PS4) {
                out->block_width = out->block_height = 8;
                out->block_bytes = (v.thick ? 256 : 64) * g.eb;
            } else {
                out->block_width = g.eb > 64 ? 1 : 64 / g.eb;
                out->block_height = 8 * v.bh;
                out->block_bytes = 512 * v.bh * v.bd;
            }
        }
        mi.offset = off;
        mi.size = v.size;
        off += v.size;
    }
    out->slice_size = off;
    out->total_size = off;
    return TEXC_OK;
}

int convert_volume_mip(texc_swizzle_mode mode, texc_format fmt,
                       uint32_t width, uint32_t height, uint32_t depth,
                       uint32_t mips, uint32_t arg, uint32_t mip,
                       const uint8_t *src, size_t src_size,
                       uint8_t *dst, size_t dst_size, bool to_linear)
{
    texc_surface_layout lay;
    const int rc = volume_layout(mode, fmt, width, height, depth, mips, arg,
                                 &lay);
    if (rc != TEXC_OK)
        return rc;
    if (mip >= lay.mip_count)
        return TEXC_ERR_INVALID_ARG;
    Grid g;
    get_grid(fmt, width, height, &g);
    VolumeMip v;
    volume_mip(mode, g, width, height, depth, mip, mips, &v);
    const uint64_t lin = (uint64_t)v.cols * v.rows * v.depth * g.eb;
    if (lay.total_size > (uint64_t)SIZE_MAX || lin > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;
    if (to_linear ? (src_size < lay.total_size || dst_size < lin)
                  : (src_size < lin || dst_size < lay.total_size))
        return TEXC_ERR_BUFFER_TOO_SMALL;
    const uint64_t base = lay.mips[mip].offset;
    if (mode == TEXC_SWIZZLE_NONE) {
        if (to_linear)
            memcpy(dst, src + (size_t)base, (size_t)lin);
        else
            memcpy(dst + (size_t)base, src, (size_t)lin);
        return TEXC_OK;
    }
    if (mode == TEXC_SWIZZLE_PS4) {
        const uint64_t tiles_x = v.pw / 8, tiles_y = v.ph / 8;
        const uint64_t slice = (uint64_t)v.pw * v.ph * g.eb;
        for (uint32_t z = 0; z < v.depth; ++z)
            for (uint32_t y = 0; y < v.rows; ++y)
                for (uint32_t x = 0; x < v.cols; ++x) {
                    const uint64_t toff = base + (v.thick
                        ? (((z / 4) * tiles_y * tiles_x + (y / 8) * tiles_x +
                            x / 8) * 256 + ps4_thick_index(g, x, y, z)) * g.eb
                        : z * slice + map_morton8x8(v.pw, x, y, g.eb));
                    const uint64_t loff =
                        (((uint64_t)z * v.rows + y) * v.cols + x) * g.eb;
                    if (toff + g.eb > lay.total_size)
                        return TEXC_ERR_BUFFER_TOO_SMALL;
                    if (to_linear)
                        memcpy(dst + (size_t)loff, src + (size_t)toff, g.eb);
                    else
                        memcpy(dst + (size_t)toff, src + (size_t)loff, g.eb);
                }
        return TEXC_OK;
    }
    const bool orin = mode == TEXC_SWIZZLE_SWITCH2;
    for (uint32_t z = 0; z < v.depth; ++z)
        for (uint32_t y = 0; y < v.rows; ++y)
            for (uint32_t x = 0; x < v.cols; ++x) {
                const uint64_t toff =
                    base + map_switch_gob_3d(v, x * g.eb, y, z, orin);
                const uint64_t loff =
                    (((uint64_t)z * v.rows + y) * v.cols + x) * g.eb;
                if (toff + g.eb > lay.total_size)
                    return TEXC_ERR_BUFFER_TOO_SMALL;
                if (to_linear)
                    memcpy(dst + (size_t)loff, src + (size_t)toff, g.eb);
                else
                    memcpy(dst + (size_t)toff, src + (size_t)loff, g.eb);
            }
    return TEXC_OK;
}

int convert_mip(texc_swizzle_mode mode, texc_format fmt,
                uint32_t width, uint32_t height,
                uint32_t mips, uint32_t slices, uint32_t arg,
                uint32_t mip, uint32_t slice,
                const uint8_t *src, size_t src_size,
                uint8_t *dst, size_t dst_size, bool dir_to_linear)
{
    texc_surface_layout lay;
    const int rc = surface_layout(mode, fmt, width, height, mips, slices,
                                  arg, &lay);
    if (rc != TEXC_OK)
        return rc;
    if (mip >= lay.mip_count || slice >= lay.slice_count)
        return TEXC_ERR_INVALID_ARG;

    if (mode == TEXC_SWIZZLE_PS5)
        return ps5::convert_mip(lay, arg, mip, slice, src, src_size,
                                dst, dst_size, dir_to_linear, false);
    if (mode == TEXC_SWIZZLE_WIIU || mode == TEXC_SWIZZLE_SWITCH ||
        mode == TEXC_SWIZZLE_SWITCH2 || mode == TEXC_SWIZZLE_PS4) {
        Grid g;
        get_grid(fmt, width, height, &g);
        if (mode == TEXC_SWIZZLE_PS4)
            return convert_mip_ps4(g, lay, mip, slice, src, src_size, dst,
                                   dst_size, dir_to_linear);
        if (mode == TEXC_SWIZZLE_WIIU)
            return convert_mip_wiiu(g, width, height, lay, arg, mip, slice,
                                    src, src_size, dst, dst_size,
                                    dir_to_linear);
        return convert_mip_switch(g, width, height, lay, arg, mip, slice,
                                  mode == TEXC_SWIZZLE_SWITCH2, src, src_size,
                                  dst, dst_size, dir_to_linear);
    }

    /* NONE: a plain copy at the mip's offset. */
    const texc_mip_layout &mi = lay.mips[mip];
    const uint64_t off = (uint64_t)slice * lay.slice_size + mi.offset;
    if (lay.total_size > (uint64_t)SIZE_MAX || mi.size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;
    if (dir_to_linear) {
        if (src_size < lay.total_size || dst_size < mi.size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memcpy(dst, src + (size_t)off, (size_t)mi.size);
    } else {
        if (src_size < mi.size || dst_size < lay.total_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        memcpy(dst + (size_t)off, src, (size_t)mi.size);
    }
    return TEXC_OK;
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
        { TEXC_SWIZZLE_SWITCH2,   "SWITCH2",   0xFFFFFFFFu },
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

        /* DX12 edge tiles are stored partial: always the linear size. */
        s = texc::swizzled_size(TEXC_SWIZZLE_DX12_64KB, TEXC_FORMAT_BC1, 2049, 2049, 0);
        check(s == format_linear_size(TEXC_FORMAT_BC1, 2049, 2049),
              "DX12 2049x2049 BC1 = linear size");
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
