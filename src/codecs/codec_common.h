/*
 * codec_common.h - internal contracts between the dispatch layer
 * (src/tex_codec.cpp) and the per-family codec modules in src/codecs/.
 *
 * Every module implements plain functions in namespace texc.
 *
 * Shared conventions (same as the public API):
 *   - RGBA8 output: R = byte 0, row-major, top-left origin, stride = w*4.
 *   - Compressed data: row-major block order, no inter-row padding.
 *   - The dispatcher validates pointers and buffer sizes BEFORE calling a
 *     module, so modules may assume:
 *       src/dst non-NULL, width/height >= 1,
 *       src_size >= texc_encoded_size(fmt, w, h) for decode,
 *       dst buffer large enough for the full output.
 *   - Partial edge blocks: decoders write only pixels inside the image;
 *     encoders replicate edge pixels to fill the block.
 *   - Return TEXC_OK or a texc_result error code.
 */
#ifndef TEXC_CODEC_COMMON_H
#define TEXC_CODEC_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../include/tex_codec.h"

namespace texc {

/* ------------------------------------------------------- shared helpers */

static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}
static inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Expand 5/6-bit channel to 8 bits (standard replicate). */
static inline uint8_t expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t expand6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t expand4(uint32_t v) { return (uint8_t)((v << 4) | v); }

/* IEEE 754 half <-> float */
float half_to_float(uint16_t h);
uint16_t float_to_half(float f);

/* Copy an up-to bw x bh decoded block (tightly packed RGBA8 in `block`)
 * into the image at block coordinates (bx, by), clipping at the edges. */
void write_block_rgba8(uint8_t *dst, uint32_t width, uint32_t height,
                       uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                       const uint8_t *block);

/* Gather a bw x bh block of source pixels at block coords (bx, by) into
 * `block` (tightly packed RGBA8), replicating edge pixels as needed. */
void read_block_rgba8(const uint8_t *src, uint32_t width, uint32_t height,
                      uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                      uint8_t *block);

/* Float variants for HDR paths (RGBA, 4 floats per pixel). */
void write_block_rgba32f(float *dst, uint32_t width, uint32_t height,
                         uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                         const float *block);
void read_block_rgba32f(const float *src, uint32_t width, uint32_t height,
                        uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                        float *block);

/* --------------------------------------------------------- BCn (bcn.cpp) */
/* Handles: BC1, BC2, BC3, BC4(+snorm), BC5(+snorm), BC6H UF16/SF16, BC7.  */

int bcn_decode(texc_format fmt, const uint8_t *src, size_t src_size,
               uint32_t width, uint32_t height, uint8_t *dst);
/* BC6H full-range decode; valid only for BC6H formats. */
int bcn_decode_f32(texc_format fmt, const uint8_t *src, size_t src_size,
                   uint32_t width, uint32_t height, float *dst);
/* opts is never NULL when called through the dispatcher (defaults filled). */
int bcn_encode(texc_format fmt, const uint8_t *src,
               uint32_t width, uint32_t height, uint8_t *dst,
               const texc_encode_options *opts);
/* BC6H HDR encode from float RGBA; valid only for BC6H formats. */
int bcn_encode_f32(texc_format fmt, const float *src,
                   uint32_t width, uint32_t height, uint8_t *dst);

/* ------------------------------------------------------ ETC/EAC (etc.cpp) */
/* Handles: ETC1, ETC2 RGB/RGBA1/RGBA8, EAC R11/RG11 (+signed).            */

int etc_decode(texc_format fmt, const uint8_t *src, size_t src_size,
               uint32_t width, uint32_t height, uint8_t *dst);
int etc_encode(texc_format fmt, const uint8_t *src,
               uint32_t width, uint32_t height, uint8_t *dst,
               const texc_encode_options *opts);

/* -------------------------------------------------------- ASTC (astc.cpp) */
/* Handles all TEXC_FORMAT_ASTC_*. HDR blocks are decoded and clamped for
 * the 8-bit path; astc_decode_f32 preserves full range.                    */

int astc_decode(texc_format fmt, const uint8_t *src, size_t src_size,
                uint32_t width, uint32_t height, uint8_t *dst);
int astc_decode_f32(texc_format fmt, const uint8_t *src, size_t src_size,
                    uint32_t width, uint32_t height, float *dst);
int astc_encode(texc_format fmt, const uint8_t *src,
                uint32_t width, uint32_t height, uint8_t *dst,
                const texc_encode_options *opts);

/* ----------------------------------------------------- PVRTC (pvrtc.cpp) */
/* Handles all TEXC_FORMAT_PVRTC*. PVRTC1 requires power-of-two dims        */
/* (return TEXC_ERR_BAD_DIMENSIONS otherwise).                              */

int pvrtc_decode(texc_format fmt, const uint8_t *src, size_t src_size,
                 uint32_t width, uint32_t height, uint8_t *dst);
int pvrtc_encode(texc_format fmt, const uint8_t *src,
                 uint32_t width, uint32_t height, uint8_t *dst,
                 const texc_encode_options *opts);

/* --------------------------------------------------------- ATC (atc.cpp) */

int atc_decode(texc_format fmt, const uint8_t *src, size_t src_size,
               uint32_t width, uint32_t height, uint8_t *dst);
int atc_encode(texc_format fmt, const uint8_t *src,
               uint32_t width, uint32_t height, uint8_t *dst,
               const texc_encode_options *opts);

/* --------------------------------------------------------- Wii (wii.cpp) */
/* GameCube/Wii GX ("TPL") formats. Paletted formats (C4/C8/C14X2) return
 * TEXC_ERR_NEEDS_PALETTE from wii_decode and TEXC_ERR_UNSUPPORTED from
 * wii_encode; decode them with wii_decode_paletted.                       */

int wii_decode(texc_format fmt, const uint8_t *src, size_t src_size,
               uint32_t width, uint32_t height, uint8_t *dst);
int wii_decode_paletted(texc_format fmt, const uint8_t *src, size_t src_size,
                        uint32_t width, uint32_t height,
                        const uint8_t *palette, size_t palette_size,
                        texc_palette_format palette_format, uint8_t *dst);
int wii_encode(texc_format fmt, const uint8_t *src,
               uint32_t width, uint32_t height, uint8_t *dst,
               const texc_encode_options *opts);
bool wii_is_paletted(texc_format fmt);
size_t wii_palette_size(texc_format fmt);      /* required palette bytes */

} /* namespace texc */

#endif /* TEXC_CODEC_COMMON_H */
