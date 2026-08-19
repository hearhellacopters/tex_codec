/*
 * image_utils.cpp - colour profile conversion, flip and crop.
 *
 * Ported behaviour from tex-decoder (github.com/hearhellacopters/tex-decoder):
 *   - profiler.ts  convertProfile(): proportional range scaling between
 *     profiles, missing alpha -> fully opaque, missing colour -> 0.
 *   - flipper.ts   flipImage()/cropImage().
 *
 * Bit packing (documented in tex_codec.h): byte-aligned channels are stored
 * in byte order with multi-byte values little-endian; the sub-byte packed
 * profiles (RGB565 & co) pack channels MSB-first in byte order like
 * tex-decoder's sequential bit reader.
 */

#include <stdlib.h>
#include <string.h>

#include "../codecs/codec_common.h"

namespace {

enum vt : uint8_t { VT_U, VT_S, VT_H, VT_F };
enum ch : uint8_t { CH_NONE = 0, CH_R, CH_G, CH_B, CH_A };

struct profile_info {
    const char *name;
    uint8_t type;                       /* vt                          */
    uint8_t chan[4];                    /* ch per position, CH_NONE end */
    uint8_t bits[4];                    /* bits per position            */
};

#define P1(n, t, c0, b0) \
    { n, t, { c0, 0, 0, 0 }, { b0, 0, 0, 0 } }
#define P2(n, t, c0, c1, b) \
    { n, t, { c0, c1, 0, 0 }, { b, b, 0, 0 } }
#define P3(n, t, c0, c1, c2, b) \
    { n, t, { c0, c1, c2, 0 }, { b, b, b, 0 } }
#define P4(n, t, c0, c1, c2, c3, b) \
    { n, t, { c0, c1, c2, c3 }, { b, b, b, b } }

const profile_info k_profiles[TEXC_PROFILE_COUNT] = {
    { nullptr, VT_U, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },        /* INVALID */
    P1("A8", VT_U, CH_A, 8), P1("R8", VT_U, CH_R, 8),
    P1("G8", VT_U, CH_G, 8), P1("B8", VT_U, CH_B, 8),
    P2("RG8", VT_U, CH_R, CH_G, 8), P2("RB8", VT_U, CH_R, CH_B, 8),
    P2("GR8", VT_U, CH_G, CH_R, 8), P2("GB8", VT_U, CH_G, CH_B, 8),
    P2("BR8", VT_U, CH_B, CH_R, 8), P2("BG8", VT_U, CH_B, CH_G, 8),
    P3("RGB8", VT_U, CH_R, CH_G, CH_B, 8),
    P3("RBG8", VT_U, CH_R, CH_B, CH_G, 8),
    P3("GRB8", VT_U, CH_G, CH_R, CH_B, 8),
    P3("GBR8", VT_U, CH_G, CH_B, CH_R, 8),
    P3("BRG8", VT_U, CH_B, CH_R, CH_G, 8),
    P3("BGR8", VT_U, CH_B, CH_G, CH_R, 8),
    P4("ARGB8", VT_U, CH_A, CH_R, CH_G, CH_B, 8),
    P4("ARBG8", VT_U, CH_A, CH_R, CH_B, CH_G, 8),
    P4("AGRB8", VT_U, CH_A, CH_G, CH_R, CH_B, 8),
    P4("AGBR8", VT_U, CH_A, CH_G, CH_B, CH_R, 8),
    P4("ABRG8", VT_U, CH_A, CH_B, CH_R, CH_G, 8),
    P4("ABGR8", VT_U, CH_A, CH_B, CH_G, CH_R, 8),
    P4("RGBA8", VT_U, CH_R, CH_G, CH_B, CH_A, 8),
    P4("RBGA8", VT_U, CH_R, CH_B, CH_G, CH_A, 8),
    P4("GRBA8", VT_U, CH_G, CH_R, CH_B, CH_A, 8),
    P4("GBRA8", VT_U, CH_G, CH_B, CH_R, CH_A, 8),
    P4("BRGA8", VT_U, CH_B, CH_R, CH_G, CH_A, 8),
    P4("BGRA8", VT_U, CH_B, CH_G, CH_R, CH_A, 8),
    { "RGB565", VT_U, { CH_R, CH_G, CH_B, 0 }, { 5, 6, 5, 0 } },
    { "BGR565", VT_U, { CH_B, CH_G, CH_R, 0 }, { 5, 6, 5, 0 } },
    { "RGBA4",  VT_U, { CH_R, CH_G, CH_B, CH_A }, { 4, 4, 4, 4 } },
    { "RGBA51", VT_U, { CH_R, CH_G, CH_B, CH_A }, { 5, 5, 5, 1 } },
    { "RGB10_A2",  VT_U, { CH_R, CH_G, CH_B, CH_A }, { 10, 10, 10, 2 } },
    { "RGB10_A2I", VT_S, { CH_R, CH_G, CH_B, CH_A }, { 10, 10, 10, 2 } },
    P1("A8I", VT_S, CH_A, 8), P1("R8I", VT_S, CH_R, 8),
    P2("RG8I", VT_S, CH_R, CH_G, 8),
    P3("RGB8I", VT_S, CH_R, CH_G, CH_B, 8),
    P4("RGBA8I", VT_S, CH_R, CH_G, CH_B, CH_A, 8),
    P4("ARGB8I", VT_S, CH_A, CH_R, CH_G, CH_B, 8),
    P3("BGR8I", VT_S, CH_B, CH_G, CH_R, 8),
    P4("BGRA8I", VT_S, CH_B, CH_G, CH_R, CH_A, 8),
    P4("ABGR8I", VT_S, CH_A, CH_B, CH_G, CH_R, 8),
    P1("A16F", VT_H, CH_A, 16), P1("R16F", VT_H, CH_R, 16),
    P2("RG16F", VT_H, CH_R, CH_G, 16),
    P3("RGB16F", VT_H, CH_R, CH_G, CH_B, 16),
    P4("RGBA16F", VT_H, CH_R, CH_G, CH_B, CH_A, 16),
    P4("ARGB16F", VT_H, CH_A, CH_R, CH_G, CH_B, 16),
    P1("R16", VT_U, CH_R, 16),
    P2("RG16", VT_U, CH_R, CH_G, 16),
    P3("RGB16", VT_U, CH_R, CH_G, CH_B, 16),
    P4("RGBA16", VT_U, CH_R, CH_G, CH_B, CH_A, 16),
    P1("A16I", VT_S, CH_A, 16), P1("R16I", VT_S, CH_R, 16),
    P2("RG16I", VT_S, CH_R, CH_G, 16),
    P3("RGB16I", VT_S, CH_R, CH_G, CH_B, 16),
    P4("RGBA16I", VT_S, CH_R, CH_G, CH_B, CH_A, 16),
    P1("A32F", VT_F, CH_A, 32), P1("R32F", VT_F, CH_R, 32),
    P2("RG32F", VT_F, CH_R, CH_G, 32),
    P3("RGB32F", VT_F, CH_R, CH_G, CH_B, 32),
    P4("RGBA32F", VT_F, CH_R, CH_G, CH_B, CH_A, 32),
    P1("A32", VT_U, CH_A, 32), P1("R32", VT_U, CH_R, 32),
    P2("RG32", VT_U, CH_R, CH_G, 32),
    P3("RGB32", VT_U, CH_R, CH_G, CH_B, 32),
    P4("RGBA32", VT_U, CH_R, CH_G, CH_B, CH_A, 32),
    P1("R32I", VT_S, CH_R, 32),
    P2("RG32I", VT_S, CH_R, CH_G, 32),
    P3("RGB32I", VT_S, CH_R, CH_G, CH_B, 32),
    P4("RGBA32I", VT_S, CH_R, CH_G, CH_B, CH_A, 32),
};

#undef P1
#undef P2
#undef P3
#undef P4

bool valid_profile(texc_pixel_profile p) {
    return p > TEXC_PROFILE_INVALID && p < TEXC_PROFILE_COUNT;
}

uint32_t profile_bits(const profile_info &pi) {
    uint32_t bits = 0;
    for (int i = 0; i < 4; i++) bits += pi.bits[i];
    return bits;
}

/* Read one channel starting at bit_off (see packing rules above). */
double read_channel(const uint8_t *px, uint32_t bit_off, uint32_t bits,
                    uint8_t type) {
    uint64_t raw = 0;
    if ((bit_off % 8) == 0 && (bits % 8) == 0) {
        const uint8_t *p = px + bit_off / 8;             /* little-endian */
        for (uint32_t i = 0; i < bits / 8; i++)
            raw |= (uint64_t)p[i] << (8 * i);
    } else {
        for (uint32_t i = 0; i < bits; i++) {            /* MSB-first */
            uint32_t b = bit_off + i;
            raw = (raw << 1) | ((px[b / 8] >> (7 - b % 8)) & 1u);
        }
    }

    switch (type) {
    case VT_U: {
        double maxv = (double)((bits >= 64 ? ~0ull : ((1ull << bits) - 1)));
        return maxv > 0 ? (double)raw / maxv : 0.0;
    }
    case VT_S: {
        int64_t sv = (int64_t)raw;
        if (bits < 64 && (raw & (1ull << (bits - 1))))
            sv -= (int64_t)(1ull << bits);               /* sign extend */
        double maxv = (double)((1ull << (bits - 1)) - 1);
        double n = maxv > 0 ? (double)sv / maxv : 0.0;
        return n < -1.0 ? -1.0 : n;
    }
    case VT_H:
        return (double)texc::half_to_float((uint16_t)raw);
    case VT_F: {
        uint32_t u = (uint32_t)raw;
        float f;
        memcpy(&f, &u, 4);
        return (double)f;
    }
    }
    return 0.0;
}

/* Write one channel starting at bit_off. */
void write_channel(uint8_t *px, uint32_t bit_off, uint32_t bits,
                   uint8_t type, double n) {
    uint64_t raw = 0;

    switch (type) {
    case VT_U: {
        double c = n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
        double maxv = (double)((1ull << bits) - 1);
        raw = (uint64_t)(c * maxv + 0.5);
        break;
    }
    case VT_S: {
        double c = n < -1.0 ? -1.0 : (n > 1.0 ? 1.0 : n);
        double maxv = (double)((1ull << (bits - 1)) - 1);
        int64_t sv = (int64_t)(c * maxv + (c >= 0 ? 0.5 : -0.5));
        raw = (uint64_t)sv & ((bits >= 64) ? ~0ull : ((1ull << bits) - 1));
        break;
    }
    case VT_H:
        raw = texc::float_to_half((float)n);
        break;
    case VT_F: {
        float f = (float)n;
        uint32_t u;
        memcpy(&u, &f, 4);
        raw = u;
        break;
    }
    }

    if ((bit_off % 8) == 0 && (bits % 8) == 0) {
        uint8_t *p = px + bit_off / 8;                   /* little-endian */
        for (uint32_t i = 0; i < bits / 8; i++)
            p[i] = (uint8_t)(raw >> (8 * i));
    } else {
        for (uint32_t i = 0; i < bits; i++) {            /* MSB-first */
            uint32_t b = bit_off + i;
            uint8_t bit = (uint8_t)((raw >> (bits - 1 - i)) & 1u);
            uint8_t mask = (uint8_t)(1u << (7 - b % 8));
            if (bit) px[b / 8] |= mask;
            else     px[b / 8] &= (uint8_t)~mask;
        }
    }
}

} // namespace

extern "C" {

const char *texc_profile_name(texc_pixel_profile profile) {
    return valid_profile(profile) ? k_profiles[profile].name : nullptr;
}

uint32_t texc_profile_bytes_per_pixel(texc_pixel_profile profile) {
    if (!valid_profile(profile)) return 0;
    return profile_bits(k_profiles[profile]) / 8;
}

int texc_convert_profile(texc_pixel_profile src_profile,
                         const uint8_t *src, size_t src_size,
                         texc_pixel_profile dst_profile,
                         uint8_t *dst, size_t dst_size,
                         uint32_t pixel_count) {
    if (!valid_profile(src_profile) || !valid_profile(dst_profile) ||
        !src || !dst)
        return TEXC_ERR_INVALID_ARG;

    const profile_info &sp = k_profiles[src_profile];
    const profile_info &dp = k_profiles[dst_profile];
    size_t sbpp = profile_bits(sp) / 8;
    size_t dbpp = profile_bits(dp) / 8;

    size_t count = pixel_count ? pixel_count : src_size / sbpp;
    if (count == 0) return TEXC_ERR_INVALID_ARG;
    if (src_size < count * sbpp || dst_size < count * dbpp)
        return TEXC_ERR_BUFFER_TOO_SMALL;

    for (size_t i = 0; i < count; i++) {
        const uint8_t *sp_px = src + i * sbpp;
        uint8_t *dp_px = dst + i * dbpp;

        /* Unpack: missing colour channels 0, missing alpha opaque.
         * Indexed by ch: v[CH_R..CH_B] = 0, v[CH_A] = 1. */
        double v[5] = { 0.0, 0.0, 0.0, 0.0, 1.0 };
        uint32_t off = 0;
        for (int c = 0; c < 4 && sp.chan[c] != CH_NONE; c++) {
            v[sp.chan[c]] = read_channel(sp_px, off, sp.bits[c], sp.type);
            off += sp.bits[c];
        }

        /* Pack. */
        off = 0;
        for (int c = 0; c < 4 && dp.chan[c] != CH_NONE; c++) {
            write_channel(dp_px, off, dp.bits[c], dp.type, v[dp.chan[c]]);
            off += dp.bits[c];
        }
    }
    return TEXC_OK;
}

int texc_flip_y(const uint8_t *src, size_t src_size,
                uint8_t *dst, size_t dst_size,
                uint32_t width, uint32_t height, uint32_t bytes_per_pixel) {
    if (!src || !dst || !width || !height || !bytes_per_pixel)
        return TEXC_ERR_INVALID_ARG;
    size_t row = (size_t)width * bytes_per_pixel;
    size_t need = row * height;
    if (src_size < need || dst_size < need) return TEXC_ERR_BUFFER_TOO_SMALL;

    if (src == dst) {                    /* in place: swap rows via temp */
        uint8_t *tmp = (uint8_t *)malloc(row);
        if (!tmp) return TEXC_ERR_OUT_OF_MEMORY;
        for (uint32_t y = 0; y < height / 2; y++) {
            uint8_t *a = dst + (size_t)y * row;
            uint8_t *b = dst + (size_t)(height - 1 - y) * row;
            memcpy(tmp, a, row);
            memcpy(a, b, row);
            memcpy(b, tmp, row);
        }
        free(tmp);
    } else {
        for (uint32_t y = 0; y < height; y++)
            memcpy(dst + (size_t)(height - 1 - y) * row,
                   src + (size_t)y * row, row);
    }
    return TEXC_OK;
}

int texc_flip_x(const uint8_t *src, size_t src_size,
                uint8_t *dst, size_t dst_size,
                uint32_t width, uint32_t height, uint32_t bytes_per_pixel) {
    if (!src || !dst || !width || !height || !bytes_per_pixel)
        return TEXC_ERR_INVALID_ARG;
    size_t bpp = bytes_per_pixel;
    size_t row = (size_t)width * bpp;
    size_t need = row * height;
    if (src_size < need || dst_size < need) return TEXC_ERR_BUFFER_TOO_SMALL;

    uint8_t *tmp = (uint8_t *)malloc(bpp);
    if (!tmp) return TEXC_ERR_OUT_OF_MEMORY;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *srow = src + (size_t)y * row;
        uint8_t *drow = dst + (size_t)y * row;
        if (src == dst) {
            for (uint32_t x = 0; x < width / 2; x++) {
                uint8_t *a = drow + (size_t)x * bpp;
                uint8_t *b = drow + (size_t)(width - 1 - x) * bpp;
                memcpy(tmp, a, bpp);
                memcpy(a, b, bpp);
                memcpy(b, tmp, bpp);
            }
        } else {
            for (uint32_t x = 0; x < width; x++)
                memcpy(drow + (size_t)(width - 1 - x) * bpp,
                       srow + (size_t)x * bpp, bpp);
        }
    }
    free(tmp);
    return TEXC_OK;
}

int texc_crop(const uint8_t *src, size_t src_size,
              uint32_t width, uint32_t height, uint32_t bytes_per_pixel,
              uint32_t x, uint32_t y,
              uint32_t crop_width, uint32_t crop_height,
              uint8_t *dst, size_t dst_size) {
    if (!src || !dst || !width || !height || !bytes_per_pixel ||
        !crop_width || !crop_height)
        return TEXC_ERR_INVALID_ARG;
    if ((uint64_t)x + crop_width > width ||
        (uint64_t)y + crop_height > height)
        return TEXC_ERR_BAD_DIMENSIONS;
    size_t bpp = bytes_per_pixel;
    if (src_size < (size_t)width * height * bpp)
        return TEXC_ERR_BUFFER_TOO_SMALL;
    if (dst_size < (size_t)crop_width * crop_height * bpp)
        return TEXC_ERR_BUFFER_TOO_SMALL;

    for (uint32_t cy = 0; cy < crop_height; cy++)
        memcpy(dst + (size_t)cy * crop_width * bpp,
               src + (((size_t)(y + cy) * width) + x) * bpp,
               (size_t)crop_width * bpp);
    return TEXC_OK;
}

} /* extern "C" */
