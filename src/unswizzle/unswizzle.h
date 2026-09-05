/*
 * unswizzle.h - internal contract for the platform swizzle module.
 *
 * Implemented in src/unswizzle/unswizzle.cpp (logic ported from the G1T
 * reference headers ref/G1T.h and ref/G1TFormatConvert.h).
 *
 * The dispatcher validates pointers before calling; the module must compute
 * required sizes itself (tile padding differs per platform) and return
 * TEXC_ERR_BUFFER_TOO_SMALL when a buffer cannot hold the result.
 */
#ifndef TEXC_UNSWIZZLE_H
#define TEXC_UNSWIZZLE_H

#include <stddef.h>
#include <stdint.h>

#include "../../include/tex_codec.h"

namespace texc {

/* Bytes occupied by the tiled representation (>= linear size, 0 invalid). */
size_t swizzled_size(texc_swizzle_mode mode, texc_format fmt,
                     uint32_t width, uint32_t height, uint32_t arg);

/* Bytes of the LINEAR representation. Normally the format's own encoded
 * size, but PS Vita raw honours the bytes-per-pixel `arg` override, so it
 * cannot be derived from the format alone. 0 if invalid. */
size_t linear_size(texc_swizzle_mode mode, texc_format fmt,
                   uint32_t width, uint32_t height, uint32_t arg);

/* dir_to_linear == true  : src tiled  -> dst linear (unswizzle)
 * dir_to_linear == false : src linear -> dst tiled  (swizzle)     */
int swizzle_convert(texc_swizzle_mode mode, texc_format fmt,
                    uint32_t width, uint32_t height,
                    const uint8_t *src, size_t src_size,
                    uint8_t *dst, size_t dst_size,
                    uint32_t arg, bool dir_to_linear);

} /* namespace texc */

#endif /* TEXC_UNSWIZZLE_H */
