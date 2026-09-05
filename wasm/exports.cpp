/*
 * exports.cpp - Emscripten glue.
 *
 * Three layers, all reachable from JavaScript (every function here and in
 * tex_codec.h carries EMSCRIPTEN_KEEPALIVE via TEXC_API, so they all appear
 * as _-prefixed exports on the module):
 *
 *   1. The generic C API from tex_codec.h (_texc_decode, _texc_encode_ex,
 *      _texc_unswizzle, ...) - format/mode passed as an integer.
 *   2. Per-format and per-mode wrappers generated below
 *      (_texc_decode_bc7, _texc_encode_etc2_rgba8,
 *       _texc_unswizzle_ps4, _texc_swizzled_size_switch, ...) - for callers
 *      that prefer named entry points over enum juggling.
 *   3. Allocation helpers (_texc_decode_alloc, _texc_encode_alloc, ...) that
 *      allocate the output buffer themselves - one call + one read.
 *
 * The ergonomic way to use all of this from JS/TS is the wrapper in
 * wasm/tex_codec_api.mjs (types in wasm/tex_codec_api.d.ts).
 */

#include "../include/tex_codec.h"

#include <stdlib.h>

extern "C" {

/* --------------------------------------------------- error of last *_alloc */

static int g_last_error = TEXC_OK;

TEXC_API int texc_last_error(void) { return g_last_error; }

/* -------------------------------------------- scalar-args encode options */

/* texc_encode with the punchthrough alpha threshold as a plain argument
 * (BC1 3-colour mode, ETC2_RGBA1). threshold 0..256; 128 = default.
 * Spares JS/FFI callers from building a texc_encode_options struct. */
TEXC_API int texc_encode_with_options(texc_format format,
                                      const uint8_t *src, size_t src_size,
                                      uint32_t width, uint32_t height,
                                      uint8_t *dst, size_t dst_size,
                                      uint32_t alpha_threshold) {
    texc_encode_options opts;
    texc_encode_options_init(&opts);
    opts.alpha_threshold = alpha_threshold;
    return texc_encode_ex(format, src, src_size, width, height,
                          dst, dst_size, &opts);
}

/* ------------------------------------------------- allocation-style API */

/* Decode to a freshly allocated RGBA8 buffer (w*h*4 bytes).
 * Returns NULL on failure; call texc_last_error() for the reason. */
TEXC_API uint8_t *texc_decode_alloc(texc_format format,
                                    const uint8_t *src, size_t src_size,
                                    uint32_t width, uint32_t height) {
    size_t out_size = texc_decoded_size(width, height);
    if (out_size == 0) { g_last_error = TEXC_ERR_INVALID_ARG; return NULL; }
    uint8_t *dst = (uint8_t *)malloc(out_size);
    if (!dst) { g_last_error = TEXC_ERR_OUT_OF_MEMORY; return NULL; }
    g_last_error = texc_decode(format, src, src_size, width, height,
                               dst, out_size);
    if (g_last_error != TEXC_OK) { free(dst); return NULL; }
    return dst;
}

/* Decode to a freshly allocated float RGBA buffer (w*h*16 bytes). */
TEXC_API float *texc_decode_f32_alloc(texc_format format,
                                      const uint8_t *src, size_t src_size,
                                      uint32_t width, uint32_t height) {
    size_t out_size = (size_t)width * height * 16;
    if (!width || !height) { g_last_error = TEXC_ERR_INVALID_ARG; return NULL; }
    float *dst = (float *)malloc(out_size);
    if (!dst) { g_last_error = TEXC_ERR_OUT_OF_MEMORY; return NULL; }
    g_last_error = texc_decode_f32(format, src, src_size, width, height,
                                   dst, out_size);
    if (g_last_error != TEXC_OK) { free(dst); return NULL; }
    return dst;
}

/* Unswizzle + decode to a freshly allocated RGBA8 buffer. */
TEXC_API uint8_t *texc_decode_swizzled_alloc(texc_swizzle_mode mode,
                                             texc_format format,
                                             const uint8_t *src,
                                             size_t src_size,
                                             uint32_t width, uint32_t height,
                                             uint32_t arg) {
    size_t out_size = texc_decoded_size(width, height);
    if (out_size == 0) { g_last_error = TEXC_ERR_INVALID_ARG; return NULL; }
    uint8_t *dst = (uint8_t *)malloc(out_size);
    if (!dst) { g_last_error = TEXC_ERR_OUT_OF_MEMORY; return NULL; }
    g_last_error = texc_decode_swizzled(mode, format, width, height,
                                        src, src_size, dst, out_size, arg);
    if (g_last_error != TEXC_OK) { free(dst); return NULL; }
    return dst;
}

/* Encode to a freshly allocated buffer; *out_size receives the byte count.
 * alpha_threshold: 128 = default behaviour. */
TEXC_API uint8_t *texc_encode_alloc(texc_format format,
                                    const uint8_t *src_rgba, size_t src_size,
                                    uint32_t width, uint32_t height,
                                    size_t *out_size) {
    size_t enc_size = texc_encoded_size(format, width, height);
    if (enc_size == 0) { g_last_error = TEXC_ERR_INVALID_ARG; return NULL; }
    uint8_t *dst = (uint8_t *)malloc(enc_size);
    if (!dst) { g_last_error = TEXC_ERR_OUT_OF_MEMORY; return NULL; }
    g_last_error = texc_encode(format, src_rgba, src_size, width, height,
                               dst, enc_size);
    if (g_last_error != TEXC_OK) { free(dst); return NULL; }
    if (out_size) *out_size = enc_size;
    return dst;
}

/* Swizzle or unswizzle to a freshly allocated buffer.
 * to_linear != 0: tiled -> linear (output texc_encoded_size bytes);
 * to_linear == 0: linear -> tiled (output texc_swizzled_size bytes).
 * *out_size receives the byte count. */
TEXC_API uint8_t *texc_swizzle_alloc(texc_swizzle_mode mode,
                                     texc_format format,
                                     const uint8_t *src, size_t src_size,
                                     uint32_t width, uint32_t height,
                                     uint32_t arg, int to_linear,
                                     size_t *out_size) {
    /* texc_unswizzled_size, not texc_encoded_size: the linear side honours
     * the PS Vita raw bytes-per-pixel `arg` override, which the format
     * alone does not describe. */
    size_t need = to_linear ? texc_unswizzled_size(mode, format, width,
                                                   height, arg)
                            : texc_swizzled_size(mode, format, width, height,
                                                 arg);
    if (need == 0) { g_last_error = TEXC_ERR_INVALID_ARG; return NULL; }
    uint8_t *dst = (uint8_t *)malloc(need);
    if (!dst) { g_last_error = TEXC_ERR_OUT_OF_MEMORY; return NULL; }
    g_last_error = to_linear
        ? texc_unswizzle(mode, format, width, height, src, src_size,
                         dst, need, arg)
        : texc_swizzle(mode, format, width, height, src, src_size,
                       dst, need, arg);
    if (g_last_error != TEXC_OK) { free(dst); return NULL; }
    if (out_size) *out_size = need;
    return dst;
}

/* ------------------------------------------------- per-format wrappers */

#define TEXC_DEF_FORMAT(suffix, FMT)                                          \
    TEXC_API int texc_decode_##suffix(const uint8_t *src, size_t src_size,    \
                                      uint32_t w, uint32_t h,                 \
                                      uint8_t *dst, size_t dst_size) {        \
        return texc_decode(FMT, src, src_size, w, h, dst, dst_size);          \
    }                                                                         \
    TEXC_API int texc_encode_##suffix(const uint8_t *src, size_t src_size,    \
                                      uint32_t w, uint32_t h,                 \
                                      uint8_t *dst, size_t dst_size,          \
                                      uint32_t alpha_threshold) {             \
        return texc_encode_with_options(FMT, src, src_size, w, h,             \
                                        dst, dst_size, alpha_threshold);      \
    }                                                                         \
    TEXC_API size_t texc_encoded_size_##suffix(uint32_t w, uint32_t h) {      \
        return texc_encoded_size(FMT, w, h);                                  \
    }

TEXC_DEF_FORMAT(rgba8,                 TEXC_FORMAT_RGBA8)
TEXC_DEF_FORMAT(bc1,                   TEXC_FORMAT_BC1)
TEXC_DEF_FORMAT(bc2,                   TEXC_FORMAT_BC2)
TEXC_DEF_FORMAT(bc3,                   TEXC_FORMAT_BC3)
TEXC_DEF_FORMAT(bc4,                   TEXC_FORMAT_BC4)
TEXC_DEF_FORMAT(bc4_snorm,             TEXC_FORMAT_BC4_SNORM)
TEXC_DEF_FORMAT(bc5,                   TEXC_FORMAT_BC5)
TEXC_DEF_FORMAT(bc5_snorm,             TEXC_FORMAT_BC5_SNORM)
TEXC_DEF_FORMAT(bc6h_uf16,             TEXC_FORMAT_BC6H_UF16)
TEXC_DEF_FORMAT(bc6h_sf16,             TEXC_FORMAT_BC6H_SF16)
TEXC_DEF_FORMAT(bc7,                   TEXC_FORMAT_BC7)
TEXC_DEF_FORMAT(etc1_rgb,              TEXC_FORMAT_ETC1_RGB)
TEXC_DEF_FORMAT(etc2_rgb,              TEXC_FORMAT_ETC2_RGB)
TEXC_DEF_FORMAT(etc2_rgba1,            TEXC_FORMAT_ETC2_RGBA1)
TEXC_DEF_FORMAT(etc2_rgba8,            TEXC_FORMAT_ETC2_RGBA8)
TEXC_DEF_FORMAT(eac_r11,               TEXC_FORMAT_EAC_R11)
TEXC_DEF_FORMAT(eac_r11_signed,        TEXC_FORMAT_EAC_R11_SIGNED)
TEXC_DEF_FORMAT(eac_rg11,              TEXC_FORMAT_EAC_RG11)
TEXC_DEF_FORMAT(eac_rg11_signed,       TEXC_FORMAT_EAC_RG11_SIGNED)
TEXC_DEF_FORMAT(pvrtc1_2bpp_rgb,       TEXC_FORMAT_PVRTC1_2BPP_RGB)
TEXC_DEF_FORMAT(pvrtc1_2bpp_rgba,      TEXC_FORMAT_PVRTC1_2BPP_RGBA)
TEXC_DEF_FORMAT(pvrtc1_4bpp_rgb,       TEXC_FORMAT_PVRTC1_4BPP_RGB)
TEXC_DEF_FORMAT(pvrtc1_4bpp_rgba,      TEXC_FORMAT_PVRTC1_4BPP_RGBA)
TEXC_DEF_FORMAT(pvrtc2_2bpp,           TEXC_FORMAT_PVRTC2_2BPP)
TEXC_DEF_FORMAT(pvrtc2_4bpp,           TEXC_FORMAT_PVRTC2_4BPP)
TEXC_DEF_FORMAT(astc_4x4,              TEXC_FORMAT_ASTC_4x4)
TEXC_DEF_FORMAT(astc_5x4,              TEXC_FORMAT_ASTC_5x4)
TEXC_DEF_FORMAT(astc_5x5,              TEXC_FORMAT_ASTC_5x5)
TEXC_DEF_FORMAT(astc_6x5,              TEXC_FORMAT_ASTC_6x5)
TEXC_DEF_FORMAT(astc_6x6,              TEXC_FORMAT_ASTC_6x6)
TEXC_DEF_FORMAT(astc_8x5,              TEXC_FORMAT_ASTC_8x5)
TEXC_DEF_FORMAT(astc_8x6,              TEXC_FORMAT_ASTC_8x6)
TEXC_DEF_FORMAT(astc_8x8,              TEXC_FORMAT_ASTC_8x8)
TEXC_DEF_FORMAT(astc_10x5,             TEXC_FORMAT_ASTC_10x5)
TEXC_DEF_FORMAT(astc_10x6,             TEXC_FORMAT_ASTC_10x6)
TEXC_DEF_FORMAT(astc_10x8,             TEXC_FORMAT_ASTC_10x8)
TEXC_DEF_FORMAT(astc_10x10,            TEXC_FORMAT_ASTC_10x10)
TEXC_DEF_FORMAT(astc_12x10,            TEXC_FORMAT_ASTC_12x10)
TEXC_DEF_FORMAT(astc_12x12,            TEXC_FORMAT_ASTC_12x12)
TEXC_DEF_FORMAT(atc_rgb,               TEXC_FORMAT_ATC_RGB)
TEXC_DEF_FORMAT(atc_rgba_explicit,     TEXC_FORMAT_ATC_RGBA_EXPLICIT)
TEXC_DEF_FORMAT(atc_rgba_interpolated, TEXC_FORMAT_ATC_RGBA_INTERPOLATED)
TEXC_DEF_FORMAT(etc1_rgb_a_atlas,      TEXC_FORMAT_ETC1_RGB_A_ATLAS)
TEXC_DEF_FORMAT(pvrtc1_4bpp_rgb_a_atlas, TEXC_FORMAT_PVRTC1_4BPP_RGB_A_ATLAS)
TEXC_DEF_FORMAT(etc2_rgb_a_atlas,      TEXC_FORMAT_ETC2_RGB_A_ATLAS)
TEXC_DEF_FORMAT(pica_etc1_rgb8,        TEXC_FORMAT_PICA_ETC1_RGB8)
TEXC_DEF_FORMAT(pica_etc1_rgb8a4,      TEXC_FORMAT_PICA_ETC1_RGB8A4)

#undef TEXC_DEF_FORMAT

/* -------------------------------------------------- per-mode swizzling
 * Direction naming:
 *   _texc_unswizzle_<mode>  : platform-tiled -> linear (BEFORE decoding)
 *   _texc_reswizzle_<mode>  : linear -> platform-tiled (put the tiling
 *                             BACK when repacking for the hardware)
 *   _texc_swizzle_<mode>    : legacy alias of _texc_reswizzle_<mode>      */

#define TEXC_DEF_SWIZZLE(suffix, MODE)                                        \
    TEXC_API int texc_unswizzle_##suffix(texc_format fmt,                     \
                                         uint32_t w, uint32_t h,              \
                                         const uint8_t *src, size_t src_size, \
                                         uint8_t *dst, size_t dst_size,       \
                                         uint32_t arg) {                      \
        return texc_unswizzle(MODE, fmt, w, h, src, src_size,                 \
                              dst, dst_size, arg);                            \
    }                                                                         \
    TEXC_API int texc_reswizzle_##suffix(texc_format fmt,                     \
                                         uint32_t w, uint32_t h,              \
                                         const uint8_t *src, size_t src_size, \
                                         uint8_t *dst, size_t dst_size,       \
                                         uint32_t arg) {                      \
        return texc_swizzle(MODE, fmt, w, h, src, src_size,                   \
                            dst, dst_size, arg);                             \
    }                                                                         \
    TEXC_API int texc_swizzle_##suffix(texc_format fmt,                       \
                                       uint32_t w, uint32_t h,                \
                                       const uint8_t *src, size_t src_size,   \
                                       uint8_t *dst, size_t dst_size,         \
                                       uint32_t arg) {                        \
        return texc_swizzle(MODE, fmt, w, h, src, src_size,                   \
                            dst, dst_size, arg);                              \
    }                                                                         \
    TEXC_API size_t texc_swizzled_size_##suffix(texc_format fmt,              \
                                                uint32_t w, uint32_t h,       \
                                                uint32_t arg) {               \
        return texc_swizzled_size(MODE, fmt, w, h, arg);                      \
    }

TEXC_DEF_SWIZZLE(ps4,       TEXC_SWIZZLE_PS4)
TEXC_DEF_SWIZZLE(ps5,       TEXC_SWIZZLE_PS5)
TEXC_DEF_SWIZZLE(switch,    TEXC_SWIZZLE_SWITCH)
TEXC_DEF_SWIZZLE(psvita,    TEXC_SWIZZLE_PSVITA)
TEXC_DEF_SWIZZLE(x360,      TEXC_SWIZZLE_X360)
TEXC_DEF_SWIZZLE(psp,       TEXC_SWIZZLE_PSP)
TEXC_DEF_SWIZZLE(n3ds,      TEXC_SWIZZLE_3DS)
TEXC_DEF_SWIZZLE(wiiu,      TEXC_SWIZZLE_WIIU)
TEXC_DEF_SWIZZLE(dx12_64kb, TEXC_SWIZZLE_DX12_64KB)

#undef TEXC_DEF_SWIZZLE

} /* extern "C" */
