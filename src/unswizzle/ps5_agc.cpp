/*
 * ps5_agc.cpp - PS5 (Prospero) texture tiling.
 *
 * The PS5 GPU is an AMD GFX10-class (RDNA) part, and its texture layout is
 * the one AMD's open-source address library ("addrlib", MIT licence,
 * https://github.com/GPUOpen-Drivers/pal - src/core/imported/addrlib,
 * gfx10addrlib.cpp / gfx10SwizzlePattern.h) computes for the SW_256B_S,
 * SW_4KB_S and SW_64KB_S "standard" swizzle modes with a single pipe:
 *
 *   - The surface is a row-major raster of fixed-size blocks (256 B, 4 KB
 *     or 64 KB). Block dimensions in elements depend only on the element
 *     size (1..16 bytes).
 *   - Inside a block, the byte offset of an element is a pure XOR/bit
 *     permutation of its x/y coordinates: the low bits address bytes within
 *     the element, the next eight bits form a 256-byte micro block, and the
 *     remaining bits interleave Y and X pairs (Y_n, X_n, Y_n+1, X_n+1 ...).
 *   - Mips smaller than half a block (and no more than blockLog2 - 4 of
 *     them) are packed into ONE shared "mip tail" block, each at a fixed
 *     micro-block position, in Z-order of decreasing mip level. The tail
 *     block is stored first, followed by the remaining mips in DESCENDING
 *     level order, so mip 0 is last. Array slices / cube faces repeat the
 *     whole chain.
 *
 * Sony's PS5 SDK exposes exactly this through sce::AgcGpuAddress
 * (TileMode::kStandard256B / kStandard4KB / kStandard64KB), and its texture
 * tool defaults sampled textures to kStandard4KB, which is what every known
 * G1T PS5 texture uses. This module was validated bit-for-bit against that
 * library's host build across element sizes, dimensions (including
 * non-power-of-two) and mip counts; see tests/ and tools/.
 */

#include "ps5_agc.h"

#include <string.h>

namespace texc {
namespace ps5 {

namespace {

/* Bit sources for the address pattern: for element size 2^bpe the byte
 * offset bits [bpe, 16) of a 64 KB block come, in order, from the entries
 * below (+n = X bit n-1, -n = Y bit n-1). A 4 KB block uses the first
 * 12 bits, a 256 B block the first 8 (addrlib GFX10_SW_PATTERN_NIBBLE01 /
 * NIBBLE2 / NIBBLE3 rows for the S modes). */
const int8_t k_pattern[5][16] = {
    /* bpe 0 (1 B)  */ { 1, 2, 3, 4, -1, -2, -3, -4, -5, 5, -6, 6, -7, 7, -8, 8 },
    /* bpe 1 (2 B)  */ { 0, 1, 2, 3, -1, -2, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8 },
    /* bpe 2 (4 B)  */ { 0, 0, 1, 2, -1, -2, -3, 3, -4, 4, -5, 5, -6, 6, -7, 7 },
    /* bpe 3 (8 B)  */ { 0, 0, 0, 1, -1, -2, 2, 3, -3, 4, -4, 5, -5, 6, -6, 7 },
    /* bpe 4 (16 B) */ { 0, 0, 0, 0, -1, -2, 1, 2, -3, 3, -4, 4, -5, 5, -6, 6 },
};

struct Mode {
    uint32_t block_log2;   /* 8, 12 or 16 */
    uint32_t bpe_log2;     /* 0..4        */
    uint32_t bw_log2, bh_log2;      /* block dims in elements (log2)   */
    uint32_t mw_log2, mh_log2;      /* 256 B micro block dims (log2)   */
    uint32_t xmask[8], ymask[8];    /* offset bits toggled by x/y bits */
};

bool block_log2_for(uint32_t tile_mode, uint32_t *out)
{
    switch (tile_mode) {
    case TEXC_PS5_TILE_DEFAULT:
    case TEXC_PS5_TILE_STANDARD_4KB:  *out = 12; return true;
    case TEXC_PS5_TILE_STANDARD_256B: *out = 8;  return true;
    case TEXC_PS5_TILE_STANDARD_64KB: *out = 16; return true;
    default: return false;
    }
}

bool bpe_log2_for(uint32_t eb, uint32_t *out)
{
    switch (eb) {
    case 1:  *out = 0; return true;
    case 2:  *out = 1; return true;
    case 4:  *out = 2; return true;
    case 8:  *out = 3; return true;
    case 16: *out = 4; return true;
    default: return false;
    }
}

bool make_mode(uint32_t tile_mode, uint32_t eb, Mode *m)
{
    memset(m, 0, sizeof *m);
    if (!block_log2_for(tile_mode, &m->block_log2) ||
        !bpe_log2_for(eb, &m->bpe_log2))
        return false;
    const int8_t *pat = k_pattern[m->bpe_log2];
    for (uint32_t bit = m->bpe_log2; bit < m->block_log2; ++bit) {
        const int src = pat[bit];
        if (src > 0) {
            m->xmask[src - 1] |= 1u << bit;
            if ((uint32_t)src > m->bw_log2) m->bw_log2 = (uint32_t)src;
            if (bit < 8 && (uint32_t)src > m->mw_log2) m->mw_log2 = (uint32_t)src;
        } else if (src < 0) {
            m->ymask[-src - 1] |= 1u << bit;
            if ((uint32_t)-src > m->bh_log2) m->bh_log2 = (uint32_t)-src;
            if (bit < 8 && (uint32_t)-src > m->mh_log2) m->mh_log2 = (uint32_t)-src;
        }
    }
    return true;
}

inline uint32_t pattern(const Mode &m, uint32_t x, uint32_t y)
{
    uint32_t off = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        if (x & (1u << i)) off ^= m.xmask[i];
        if (y & (1u << i)) off ^= m.ymask[i];
    }
    return off;
}

inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
inline uint32_t shift_ceil(uint32_t v, uint32_t s) { return (v + (1u << s) - 1) >> s; }
inline uint32_t max1(uint32_t v) { return v ? v : 1; }

/* Position of tail entry `m` (0 = smallest mip) as a micro-block index:
 * the 256 B micro blocks of the tail block are visited in Z-order and
 * tail mips occupy them from the largest index down (addrlib
 * ComputeSurfaceInfoMacroTiled: mipOffset = m > 6 ? 16 << m : m << 8). */
void tail_coord(const Mode &mode, uint32_t m, uint32_t *tx, uint32_t *ty)
{
    const uint32_t moff = (m > 6) ? (16u << m) : (m << 8);
    uint32_t mx = 0, my = 0;
    for (uint32_t i = 0; i < 6; ++i) {
        mx |= ((moff >> (9 + 2 * i)) & 1u) << i;
        my |= ((moff >> (8 + 2 * i)) & 1u) << i;
    }
    *tx = mx << mode.mw_log2;
    *ty = my << mode.mh_log2;
}

} /* namespace */

bool tile_mode_supported(uint32_t tile_mode)
{
    uint32_t dummy;
    return block_log2_for(tile_mode, &dummy);
}

int surface_layout(uint32_t w, uint32_t h, uint32_t bw_px, uint32_t bh_px,
                   uint32_t eb, uint32_t mips, uint32_t slices,
                   uint32_t tile_mode, texc_surface_layout *out)
{
    if (!out || !w || !h || !bw_px || !bh_px || !mips ||
        mips > TEXC_MAX_MIPS || !slices)
        return TEXC_ERR_INVALID_ARG;
    /* Like the hardware: at most 1 + log2(max dimension) levels. */
    {
        uint32_t maxd = w > h ? w : h, levels = 1;
        while (maxd > 1) { maxd >>= 1; ++levels; }
        if (mips > levels)
            return TEXC_ERR_INVALID_ARG;
    }
    Mode mode;
    if (!make_mode(tile_mode, eb, &mode))
        return TEXC_ERR_UNSUPPORTED;
    const uint32_t ew = (w + bw_px - 1) / bw_px;
    const uint32_t eh = (h + bh_px - 1) / bh_px;

    memset(out, 0, sizeof *out);
    out->mip_count = mips;
    out->slice_count = slices;
    out->block_width = 1u << mode.bw_log2;
    out->block_height = 1u << mode.bh_log2;
    out->block_bytes = 1u << mode.block_log2;
    out->element_bytes = eb;
    out->first_mip_in_tail = mips;

    const uint32_t bw = out->block_width, bh = out->block_height;
    const uint64_t block_bytes = out->block_bytes;
    const bool macro = mode.block_log2 > 8;

    /* Block padding and the tail test use ceil(elements0 / 2^m); the
     * element count actually read for a mip comes from its pixel size,
     * ceil((w >> m) / texelsPerElement). The two differ for some
     * non-power-of-two sizes; this mirrors Sony's library / addrlib. */
    uint32_t first_in_tail = mips;
    uint64_t slice_size = 0;
    if (macro && mips > 1) {
        const uint32_t max_tail = mode.block_log2 - 4;  /* 8 or 12 */
        const uint32_t tail_w = bw >> 1, tail_h = bh;
        for (uint32_t m = 0; m < mips; ++m) {
            const uint32_t wc = max1(shift_ceil(ew, m));
            const uint32_t hc = max1(shift_ceil(eh, m));
            if (wc <= tail_w && hc <= tail_h && (mips - m) <= max_tail) {
                first_in_tail = m;
                slice_size += block_bytes;
                break;
            }
            texc_mip_layout &mi = out->mips[m];
            mi.padded_width = align_up(wc, bw);
            mi.padded_height = align_up(hc, bh);
            mi.size = (uint64_t)mi.padded_width * mi.padded_height * eb;
            slice_size += mi.size;
        }
    } else {
        /* 256 B micro mode (or a single mip): no tail, every level is its
         * own block raster. */
        for (uint32_t m = 0; m < mips; ++m) {
            const uint32_t wc = max1(shift_ceil(ew, m));
            const uint32_t hc = max1(shift_ceil(eh, m));
            texc_mip_layout &mi = out->mips[m];
            mi.padded_width = align_up(wc, bw);
            mi.padded_height = align_up(hc, bh);
            mi.size = (uint64_t)mi.padded_width * mi.padded_height * eb;
            slice_size += mi.size;
        }
    }
    out->first_mip_in_tail = first_in_tail;

    /* Offsets: tail block first (offset 0), then mips first_in_tail-1 .. 0. */
    uint64_t off = (first_in_tail < mips) ? block_bytes : 0;
    for (int m = (int)first_in_tail - 1; m >= 0; --m) {
        out->mips[m].offset = off;
        off += out->mips[m].size;
    }
    for (uint32_t m = 0; m < mips; ++m) {
        texc_mip_layout &mi = out->mips[m];
        mi.width = max1(((w >> m) + bw_px - 1) / bw_px);
        mi.height = max1(((h >> m) + bh_px - 1) / bh_px);
        if (m >= first_in_tail) {
            mi.in_tail = 1;
            mi.offset = 0;
            mi.size = block_bytes;
            mi.padded_width = bw;
            mi.padded_height = bh;
            const uint32_t max_tail = mode.block_log2 - 4;
            tail_coord(mode, max_tail - 1 - (m - first_in_tail),
                       &mi.tail_x, &mi.tail_y);
        }
    }
    out->slice_size = slice_size;
    out->total_size = slice_size * slices;
    return TEXC_OK;
}

int convert_mip(const texc_surface_layout &lay, uint32_t tile_mode,
                uint32_t mip, uint32_t slice,
                const uint8_t *src, size_t src_size,
                uint8_t *dst, size_t dst_size,
                bool to_linear, bool clear)
{
    if (mip >= lay.mip_count || slice >= lay.slice_count)
        return TEXC_ERR_INVALID_ARG;
    Mode mode;
    if (!make_mode(tile_mode, lay.element_bytes, &mode))
        return TEXC_ERR_UNSUPPORTED;

    const texc_mip_layout &mi = lay.mips[mip];
    const uint32_t eb = lay.element_bytes;
    const uint64_t lin_size = (uint64_t)mi.width * mi.height * eb;
    const uint64_t surf_size = lay.total_size;
    if (lin_size > (uint64_t)SIZE_MAX || surf_size > (uint64_t)SIZE_MAX)
        return TEXC_ERR_INVALID_ARG;
    if (to_linear) {
        if (src_size < surf_size || dst_size < lin_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
    } else {
        if (src_size < lin_size || dst_size < surf_size)
            return TEXC_ERR_BUFFER_TOO_SMALL;
        if (clear)
            memset(dst, 0, (size_t)surf_size);
    }

    const uint64_t base = (uint64_t)slice * lay.slice_size + mi.offset;
    const uint32_t width_in_blocks = mi.padded_width >> mode.bw_log2;
    const uint32_t tx = mi.in_tail ? mi.tail_x : 0;
    const uint32_t ty = mi.in_tail ? mi.tail_y : 0;

    for (uint32_t y = 0; y < mi.height; ++y) {
        const uint64_t row_block = (uint64_t)(y >> mode.bh_log2) * width_in_blocks;
        const uint32_t ypat = pattern(mode, 0, y + ty);
        for (uint32_t x = 0; x < mi.width; ++x) {
            const uint64_t block = row_block + (x >> mode.bw_log2);
            const uint64_t toff = base + (block << mode.block_log2) +
                                  (ypat ^ pattern(mode, x + tx, 0));
            const uint64_t loff = ((uint64_t)y * mi.width + x) * eb;
            if (toff + eb > surf_size)
                return TEXC_ERR_BUFFER_TOO_SMALL;
            if (to_linear)
                memcpy(dst + (size_t)loff, src + (size_t)toff, eb);
            else
                memcpy(dst + (size_t)toff, src + (size_t)loff, eb);
        }
    }
    return TEXC_OK;
}

} /* namespace ps5 */
} /* namespace texc */
