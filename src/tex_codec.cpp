/*
 * tex_codec.cpp - public C API entry points, format metadata, argument
 * validation and dispatch into the per-family codec modules.
 */

#include <stdlib.h>
#include <string.h>

#include "../include/tex_codec.h"
#include "codecs/codec_common.h"
#include "unswizzle/unswizzle.h"

/* ------------------------------------------------------- format metadata */

namespace {

struct format_info {
    const char *name;
    uint32_t block_w;
    uint32_t block_h;
    uint32_t block_bytes;
};

const format_info k_formats[TEXC_FORMAT_COUNT] = {
    /* INVALID              */ { nullptr,               0,  0,  0  },
    /* RGBA8                */ { "RGBA8",               1,  1,  4  },
    /* BC1                  */ { "BC1",                 4,  4,  8  },
    /* BC2                  */ { "BC2",                 4,  4,  16 },
    /* BC3                  */ { "BC3",                 4,  4,  16 },
    /* BC4                  */ { "BC4",                 4,  4,  8  },
    /* BC4_SNORM            */ { "BC4_SNORM",           4,  4,  8  },
    /* BC5                  */ { "BC5",                 4,  4,  16 },
    /* BC5_SNORM            */ { "BC5_SNORM",           4,  4,  16 },
    /* BC6H_UF16            */ { "BC6H_UF16",           4,  4,  16 },
    /* BC6H_SF16            */ { "BC6H_SF16",           4,  4,  16 },
    /* BC7                  */ { "BC7",                 4,  4,  16 },
    /* ETC1_RGB             */ { "ETC1_RGB",            4,  4,  8  },
    /* ETC2_RGB             */ { "ETC2_RGB",            4,  4,  8  },
    /* ETC2_RGBA1           */ { "ETC2_RGBA1",          4,  4,  8  },
    /* ETC2_RGBA8           */ { "ETC2_RGBA8",          4,  4,  16 },
    /* EAC_R11              */ { "EAC_R11",             4,  4,  8  },
    /* EAC_R11_SIGNED       */ { "EAC_R11_SIGNED",      4,  4,  8  },
    /* EAC_RG11             */ { "EAC_RG11",            4,  4,  16 },
    /* EAC_RG11_SIGNED      */ { "EAC_RG11_SIGNED",     4,  4,  16 },
    /* PVRTC1_2BPP_RGB      */ { "PVRTC1_2BPP_RGB",     8,  4,  8  },
    /* PVRTC1_2BPP_RGBA     */ { "PVRTC1_2BPP_RGBA",    8,  4,  8  },
    /* PVRTC1_4BPP_RGB      */ { "PVRTC1_4BPP_RGB",     4,  4,  8  },
    /* PVRTC1_4BPP_RGBA     */ { "PVRTC1_4BPP_RGBA",    4,  4,  8  },
    /* PVRTC2_2BPP          */ { "PVRTC2_2BPP",         8,  4,  8  },
    /* PVRTC2_4BPP          */ { "PVRTC2_4BPP",         4,  4,  8  },
    /* ASTC_4x4             */ { "ASTC_4x4",            4,  4,  16 },
    /* ASTC_5x4             */ { "ASTC_5x4",            5,  4,  16 },
    /* ASTC_5x5             */ { "ASTC_5x5",            5,  5,  16 },
    /* ASTC_6x5             */ { "ASTC_6x5",            6,  5,  16 },
    /* ASTC_6x6             */ { "ASTC_6x6",            6,  6,  16 },
    /* ASTC_8x5             */ { "ASTC_8x5",            8,  5,  16 },
    /* ASTC_8x6             */ { "ASTC_8x6",            8,  6,  16 },
    /* ASTC_8x8             */ { "ASTC_8x8",            8,  8,  16 },
    /* ASTC_10x5            */ { "ASTC_10x5",           10, 5,  16 },
    /* ASTC_10x6            */ { "ASTC_10x6",           10, 6,  16 },
    /* ASTC_10x8            */ { "ASTC_10x8",           10, 8,  16 },
    /* ASTC_10x10           */ { "ASTC_10x10",          10, 10, 16 },
    /* ASTC_12x10           */ { "ASTC_12x10",          12, 10, 16 },
    /* ASTC_12x12           */ { "ASTC_12x12",          12, 12, 16 },
    /* ATC_RGB              */ { "ATC_RGB",             4,  4,  8  },
    /* ATC_RGBA_EXPLICIT    */ { "ATC_RGBA_EXPLICIT",   4,  4,  16 },
    /* ATC_RGBA_INTERPOLATED*/ { "ATC_RGBA_INTERPOLATED",4, 4,  16 },
    /* Alpha-atlas formats: 4x4 blocks of the base codec, but 8bpp effective
     * because the data holds the image twice (RGB + alpha plane). Sizes are
     * special-cased in texc_encoded_size (base codec at double height). */
    /* ETC1_RGB_A_ATLAS     */ { "ETC1_RGB_A_ATLAS",        4, 4, 16 },
    /* PVRTC1_4BPP_RGB_A_ATLAS*/{ "PVRTC1_4BPP_RGB_A_ATLAS",4, 4, 16 },
    /* ETC2_RGB_A_ATLAS     */ { "ETC2_RGB_A_ATLAS",        4, 4, 16 },
};

inline bool valid_format(texc_format f) {
    return f > TEXC_FORMAT_INVALID && f < TEXC_FORMAT_COUNT;
}

enum class family { raw, bcn, etc, astc, pvrtc, atc, atlas };

family format_family(texc_format f) {
    if (f == TEXC_FORMAT_RGBA8) return family::raw;
    if (f >= TEXC_FORMAT_BC1 && f <= TEXC_FORMAT_BC7) return family::bcn;
    if (f >= TEXC_FORMAT_ETC1_RGB && f <= TEXC_FORMAT_EAC_RG11_SIGNED) return family::etc;
    if (f >= TEXC_FORMAT_PVRTC1_2BPP_RGB && f <= TEXC_FORMAT_PVRTC2_4BPP) return family::pvrtc;
    if (f >= TEXC_FORMAT_ASTC_4x4 && f <= TEXC_FORMAT_ASTC_12x12) return family::astc;
    if (f >= TEXC_FORMAT_ETC1_RGB_A_ATLAS && f <= TEXC_FORMAT_ETC2_RGB_A_ATLAS)
        return family::atlas;
    return family::atc;
}

/* Base single-plane codec of a G1T alpha-atlas format. */
texc_format atlas_base(texc_format f) {
    switch (f) {
    case TEXC_FORMAT_ETC1_RGB_A_ATLAS:        return TEXC_FORMAT_ETC1_RGB;
    case TEXC_FORMAT_PVRTC1_4BPP_RGB_A_ATLAS: return TEXC_FORMAT_PVRTC1_4BPP_RGB;
    case TEXC_FORMAT_ETC2_RGB_A_ATLAS:        return TEXC_FORMAT_ETC2_RGB;
    default:                                  return TEXC_FORMAT_INVALID;
    }
}

/* Atlas data is the base codec at double height; both the size math and the
 * platform swizzles operate on that double-height image (KTGL does exactly
 * this: HEIGHT *= 2 before decode/untile, HEIGHT /= 2 after the fold). */
inline bool atlas_dims(texc_format f, uint32_t h, texc_format *base,
                       uint32_t *h2) {
    if (format_family(f) != family::atlas) return false;
    *base = atlas_base(f);
    *h2 = h * 2;                       /* h <= 2^31 checked by callers' size math */
    return true;
}

inline bool is_hdr_format(texc_format f) {
    return f == TEXC_FORMAT_BC6H_UF16 || f == TEXC_FORMAT_BC6H_SF16;
}

} // namespace

/* ------------------------------------------------------------------ query */

extern "C" {

uint32_t texc_version(void) {
    return (TEXC_VERSION_MAJOR << 16) | (TEXC_VERSION_MINOR << 8) |
           TEXC_VERSION_PATCH;
}

const char *texc_format_name(texc_format format) {
    return valid_format(format) ? k_formats[format].name : nullptr;
}

const char *texc_result_str(int result) {
    switch (result) {
    case TEXC_OK:                   return "ok";
    case TEXC_ERR_INVALID_ARG:      return "invalid argument";
    case TEXC_ERR_BUFFER_TOO_SMALL: return "buffer too small";
    case TEXC_ERR_UNSUPPORTED:      return "unsupported format or operation";
    case TEXC_ERR_BAD_DATA:         return "malformed compressed data";
    case TEXC_ERR_OUT_OF_MEMORY:    return "out of memory";
    case TEXC_ERR_BAD_DIMENSIONS:   return "unsupported image dimensions";
    default:                        return "unknown error";
    }
}

int texc_block_dims(texc_format format, uint32_t *block_width,
                    uint32_t *block_height, uint32_t *block_bytes) {
    if (!valid_format(format)) return TEXC_ERR_INVALID_ARG;
    const format_info &fi = k_formats[format];
    if (block_width)  *block_width  = fi.block_w;
    if (block_height) *block_height = fi.block_h;
    if (block_bytes)  *block_bytes  = fi.block_bytes;
    return TEXC_OK;
}

size_t texc_encoded_size(texc_format format, uint32_t width, uint32_t height) {
    if (!valid_format(format) || width == 0 || height == 0) return 0;
    {   /* alpha-atlas: base codec at double height */
        texc_format base; uint32_t h2;
        if (height <= 0x7FFFFFFFu && atlas_dims(format, height, &base, &h2))
            return texc_encoded_size(base, width, h2);
        if (height > 0x7FFFFFFFu && format_family(format) == family::atlas)
            return 0;
    }
    const format_info &fi = k_formats[format];
    uint64_t bx = ((uint64_t)width  + fi.block_w - 1) / fi.block_w;
    uint64_t by = ((uint64_t)height + fi.block_h - 1) / fi.block_h;
    /* PVRTC1 hardware requires at least a 2x2 grid of blocks. */
    if (format >= TEXC_FORMAT_PVRTC1_2BPP_RGB &&
        format <= TEXC_FORMAT_PVRTC1_4BPP_RGBA) {
        if (bx < 2) bx = 2;
        if (by < 2) by = 2;
    }
    uint64_t total = bx * by * fi.block_bytes;
    if (total > SIZE_MAX) return 0;
    return (size_t)total;
}

size_t texc_decoded_size(uint32_t width, uint32_t height) {
    uint64_t total = (uint64_t)width * height * 4;
    if (width == 0 || height == 0 || total > SIZE_MAX) return 0;
    return (size_t)total;
}

int texc_can_decode(texc_format format) {
    return valid_format(format) ? 1 : 0;
}

int texc_can_encode(texc_format format) {
    return valid_format(format) ? 1 : 0;
}

/* -------------------------------------------------------- decode / encode */

int texc_decode(texc_format format, const uint8_t *src, size_t src_size,
                uint32_t width, uint32_t height,
                uint8_t *dst, size_t dst_size) {
    if (!valid_format(format) || !src || !dst || !width || !height)
        return TEXC_ERR_INVALID_ARG;
    if (src_size < texc_encoded_size(format, width, height))
        return TEXC_ERR_BUFFER_TOO_SMALL;
    size_t need = texc_decoded_size(width, height);
    if (need == 0) return TEXC_ERR_INVALID_ARG;
    if (dst_size < need) return TEXC_ERR_BUFFER_TOO_SMALL;

    switch (format_family(format)) {
    case family::raw:
        memcpy(dst, src, need);
        return TEXC_OK;
    case family::bcn:
        return texc::bcn_decode(format, src, src_size, width, height, dst);
    case family::etc:
        return texc::etc_decode(format, src, src_size, width, height, dst);
    case family::astc:
        return texc::astc_decode(format, src, src_size, width, height, dst);
    case family::pvrtc:
        return texc::pvrtc_decode(format, src, src_size, width, height, dst);
    case family::atc:
        return texc::atc_decode(format, src, src_size, width, height, dst);
    case family::atlas: {
        /* Decode the double-height base image, keep the top half as RGB and
         * fold the bottom half's R channel into A (KTGL bHasAlphaAtlas). */
        texc_format base; uint32_t h2;
        if (height > 0x7FFFFFFFu || !atlas_dims(format, height, &base, &h2))
            return TEXC_ERR_INVALID_ARG;
        size_t full = texc_decoded_size(width, h2);
        if (full == 0) return TEXC_ERR_INVALID_ARG;
        uint8_t *tmp = (uint8_t *)malloc(full);
        if (!tmp) return TEXC_ERR_OUT_OF_MEMORY;
        int rc = texc_decode(base, src, src_size, width, h2, tmp, full);
        if (rc == TEXC_OK) {
            size_t count = (size_t)width * height;
            memcpy(dst, tmp, count * 4);
            const uint8_t *alpha_plane = tmp + count * 4;
            for (size_t j = 0; j < count; j++)
                dst[4 * j + 3] = alpha_plane[4 * j];
        }
        free(tmp);
        return rc;
    }
    }
    return TEXC_ERR_UNSUPPORTED;
}

int texc_decode_f32(texc_format format, const uint8_t *src, size_t src_size,
                    uint32_t width, uint32_t height,
                    float *dst, size_t dst_size) {
    if (!valid_format(format) || !src || !dst || !width || !height)
        return TEXC_ERR_INVALID_ARG;
    if (src_size < texc_encoded_size(format, width, height))
        return TEXC_ERR_BUFFER_TOO_SMALL;
    uint64_t need = (uint64_t)width * height * 16;
    if (need > SIZE_MAX || dst_size < (size_t)need)
        return TEXC_ERR_BUFFER_TOO_SMALL;

    /* Native float paths. */
    if (is_hdr_format(format))
        return texc::bcn_decode_f32(format, src, src_size, width, height, dst);
    if (format_family(format) == family::astc)
        return texc::astc_decode_f32(format, src, src_size, width, height, dst);

    /* LDR formats: decode 8-bit then normalise. */
    size_t n8 = texc_decoded_size(width, height);
    uint8_t *tmp = (uint8_t *)malloc(n8);
    if (!tmp) return TEXC_ERR_OUT_OF_MEMORY;
    int rc = texc_decode(format, src, src_size, width, height, tmp, n8);
    if (rc == TEXC_OK) {
        size_t count = (size_t)width * height * 4;
        for (size_t i = 0; i < count; i++)
            dst[i] = tmp[i] * (1.0f / 255.0f);
    }
    free(tmp);
    return rc;
}

void texc_encode_options_init(texc_encode_options *opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->struct_size = (uint32_t)sizeof(*opts);
    opts->alpha_threshold = 128;
}

int texc_encode_ex(texc_format format, const uint8_t *src, size_t src_size,
                   uint32_t width, uint32_t height,
                   uint8_t *dst, size_t dst_size,
                   const texc_encode_options *opts) {
    if (!valid_format(format) || !src || !dst || !width || !height)
        return TEXC_ERR_INVALID_ARG;
    size_t in_need = texc_decoded_size(width, height);
    size_t out_need = texc_encoded_size(format, width, height);
    if (in_need == 0 || out_need == 0) return TEXC_ERR_INVALID_ARG;
    if (src_size < in_need || dst_size < out_need)
        return TEXC_ERR_BUFFER_TOO_SMALL;

    texc_encode_options defaults;
    texc_encode_options_init(&defaults);
    if (!opts) {
        opts = &defaults;
    } else {
        if (opts->struct_size < sizeof(uint32_t) * 2 || opts->flags != 0)
            return TEXC_ERR_INVALID_ARG;
        /* Accept older/shorter structs: copy what the caller provided over
         * the defaults so new fields keep their default values. */
        size_t have = opts->struct_size < sizeof(defaults) ? opts->struct_size
                                                           : sizeof(defaults);
        memcpy(&defaults, opts, have);
        defaults.struct_size = (uint32_t)sizeof(defaults);
        opts = &defaults;
    }

    switch (format_family(format)) {
    case family::raw:
        memcpy(dst, src, in_need);
        return TEXC_OK;
    case family::bcn:
        return texc::bcn_encode(format, src, width, height, dst, opts);
    case family::etc:
        return texc::etc_encode(format, src, width, height, dst, opts);
    case family::astc:
        return texc::astc_encode(format, src, width, height, dst, opts);
    case family::pvrtc:
        return texc::pvrtc_encode(format, src, width, height, dst, opts);
    case family::atc:
        return texc::atc_encode(format, src, width, height, dst, opts);
    case family::atlas: {
        /* Build the double-height base image: RGB on top, the alpha channel
         * replicated as grayscale below, then encode as the base codec. */
        texc_format base; uint32_t h2;
        if (height > 0x7FFFFFFFu || !atlas_dims(format, height, &base, &h2))
            return TEXC_ERR_INVALID_ARG;
        size_t full = texc_decoded_size(width, h2);
        if (full == 0) return TEXC_ERR_INVALID_ARG;
        uint8_t *tmp = (uint8_t *)malloc(full);
        if (!tmp) return TEXC_ERR_OUT_OF_MEMORY;
        size_t count = (size_t)width * height;
        memcpy(tmp, src, count * 4);
        uint8_t *alpha_plane = tmp + count * 4;
        for (size_t j = 0; j < count; j++) {
            uint8_t a = src[4 * j + 3];
            alpha_plane[4 * j + 0] = a;
            alpha_plane[4 * j + 1] = a;
            alpha_plane[4 * j + 2] = a;
            alpha_plane[4 * j + 3] = 255;
        }
        int rc = texc_encode_ex(base, tmp, full, width, h2, dst, dst_size,
                                opts);
        free(tmp);
        return rc;
    }
    }
    return TEXC_ERR_UNSUPPORTED;
}

int texc_encode(texc_format format, const uint8_t *src, size_t src_size,
                uint32_t width, uint32_t height,
                uint8_t *dst, size_t dst_size) {
    return texc_encode_ex(format, src, src_size, width, height,
                          dst, dst_size, nullptr);
}

int texc_encode_f32(texc_format format, const float *src, size_t src_size,
                    uint32_t width, uint32_t height,
                    uint8_t *dst, size_t dst_size) {
    if (!valid_format(format) || !src || !dst || !width || !height)
        return TEXC_ERR_INVALID_ARG;
    uint64_t in_need = (uint64_t)width * height * 16;
    size_t out_need = texc_encoded_size(format, width, height);
    if (in_need > SIZE_MAX || out_need == 0) return TEXC_ERR_INVALID_ARG;
    if (src_size < (size_t)in_need || dst_size < out_need)
        return TEXC_ERR_BUFFER_TOO_SMALL;

    if (is_hdr_format(format))
        return texc::bcn_encode_f32(format, src, width, height, dst);

    /* LDR formats: clamp to 8-bit then encode. */
    size_t n8 = texc_decoded_size(width, height);
    uint8_t *tmp = (uint8_t *)malloc(n8);
    if (!tmp) return TEXC_ERR_OUT_OF_MEMORY;
    size_t count = (size_t)width * height * 4;
    for (size_t i = 0; i < count; i++)
        tmp[i] = texc::clamp_u8((int)(texc::clamp01(src[i]) * 255.0f + 0.5f));
    int rc = texc_encode(format, tmp, n8, width, height, dst, dst_size);
    free(tmp);
    return rc;
}

/* -------------------------------------------------------------- unswizzle */

/* Alpha-atlas data is tiled as ONE double-height image of the base codec
 * (KTGL doubles HEIGHT before untiling), so swizzle operations map to the
 * base format at 2 * height. */
static bool swizzle_target(texc_format &format, uint32_t &height) {
    texc_format base; uint32_t h2;
    if (height > 0x7FFFFFFFu && format_family(format) == family::atlas)
        return false;
    if (atlas_dims(format, height, &base, &h2)) {
        format = base;
        height = h2;
    }
    return true;
}

size_t texc_swizzled_size(texc_swizzle_mode mode, texc_format format,
                          uint32_t width, uint32_t height, uint32_t arg) {
    if (mode < 0 || mode >= TEXC_SWIZZLE_MODE_COUNT || !valid_format(format))
        return 0;
    if (!swizzle_target(format, height)) return 0;
    return texc::swizzled_size(mode, format, width, height, arg);
}

int texc_unswizzle(texc_swizzle_mode mode, texc_format format,
                   uint32_t width, uint32_t height,
                   const uint8_t *src, size_t src_size,
                   uint8_t *dst, size_t dst_size, uint32_t arg) {
    if (mode < 0 || mode >= TEXC_SWIZZLE_MODE_COUNT || !valid_format(format) ||
        !src || !dst || !width || !height)
        return TEXC_ERR_INVALID_ARG;
    if (!swizzle_target(format, height)) return TEXC_ERR_INVALID_ARG;
    return texc::swizzle_convert(mode, format, width, height,
                                 src, src_size, dst, dst_size, arg, true);
}

int texc_swizzle(texc_swizzle_mode mode, texc_format format,
                 uint32_t width, uint32_t height,
                 const uint8_t *src, size_t src_size,
                 uint8_t *dst, size_t dst_size, uint32_t arg) {
    if (mode < 0 || mode >= TEXC_SWIZZLE_MODE_COUNT || !valid_format(format) ||
        !src || !dst || !width || !height)
        return TEXC_ERR_INVALID_ARG;
    if (!swizzle_target(format, height)) return TEXC_ERR_INVALID_ARG;
    return texc::swizzle_convert(mode, format, width, height,
                                 src, src_size, dst, dst_size, arg, false);
}

int texc_reswizzle(texc_swizzle_mode mode, texc_format format,
                   uint32_t width, uint32_t height,
                   const uint8_t *src, size_t src_size,
                   uint8_t *dst, size_t dst_size, uint32_t arg) {
    /* Alias of texc_swizzle: linear -> platform-tiled. */
    return texc_swizzle(mode, format, width, height,
                        src, src_size, dst, dst_size, arg);
}

int texc_decode_swizzled(texc_swizzle_mode mode, texc_format format,
                         uint32_t width, uint32_t height,
                         const uint8_t *src, size_t src_size,
                         uint8_t *dst, size_t dst_size, uint32_t arg) {
    if (mode == TEXC_SWIZZLE_NONE)
        return texc_decode(format, src, src_size, width, height, dst, dst_size);

    size_t linear_size = texc_encoded_size(format, width, height);
    if (linear_size == 0) return TEXC_ERR_INVALID_ARG;
    uint8_t *linear = (uint8_t *)malloc(linear_size);
    if (!linear) return TEXC_ERR_OUT_OF_MEMORY;

    int rc = texc_unswizzle(mode, format, width, height,
                            src, src_size, linear, linear_size, arg);
    if (rc == TEXC_OK)
        rc = texc_decode(format, linear, linear_size, width, height,
                         dst, dst_size);
    free(linear);
    return rc;
}

/* ---------------------------------------------------------------- memory */

void *texc_alloc(size_t size) { return malloc(size); }
void  texc_free(void *ptr)    { free(ptr); }

} /* extern "C" */
