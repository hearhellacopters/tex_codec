/*
 * ps5_agc.h - PS5 (Prospero) texture tiling: AMD GFX10 "standard" swizzle
 * block layout with mip-tail packing.
 *
 * Implemented in ps5_agc.cpp; used by unswizzle.cpp for TEXC_SWIZZLE_PS5.
 */
#ifndef TEXC_PS5_AGC_H
#define TEXC_PS5_AGC_H

#include <stddef.h>
#include <stdint.h>

#include "../../include/tex_codec.h"

namespace texc {
namespace ps5 {

/* True for the tile modes this module implements (not LEGACY). */
bool tile_mode_supported(uint32_t tile_mode);

/* Compute the surface layout for a w x h PIXEL texture whose elements are
 * bw x bh pixels and eb bytes, with `mips` levels and `slices` layers.
 * Returns TEXC_OK or an error code. */
int surface_layout(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh,
                   uint32_t eb, uint32_t mips, uint32_t slices,
                   uint32_t tile_mode, texc_surface_layout *out);

/* Move one mip of one slice between the tiled surface and a linear buffer.
 *   to_linear == true : src = whole surface, dst = linear mip
 *   to_linear == false: src = linear mip,    dst = whole surface (only the
 *                       mip's elements are written; `clear` zero-fills the
 *                       whole surface first)
 * Buffer sizes are validated against the layout. */
int convert_mip(const texc_surface_layout &lay, uint32_t tile_mode,
                uint32_t mip, uint32_t slice,
                const uint8_t *src, size_t src_size,
                uint8_t *dst, size_t dst_size,
                bool to_linear, bool clear);

} /* namespace ps5 */
} /* namespace texc */

#endif /* TEXC_PS5_AGC_H */
