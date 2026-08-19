/* common.cpp - shared helper implementations (see codec_common.h). */

#include "codec_common.h"

namespace texc {

float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) & 1u;
    uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
    uint32_t man  = (uint32_t)h & 0x3FFu;
    uint32_t f;

    if (exp == 0) {
        if (man == 0) {
            f = sign << 31;                       /* +/- 0 */
        } else {
            /* subnormal: normalise */
            exp = 127 - 15 + 1;
            while ((man & 0x400u) == 0) { man <<= 1; exp--; }
            man &= 0x3FFu;
            f = (sign << 31) | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        f = (sign << 31) | 0x7F800000u | (man << 13);  /* inf / nan */
    } else {
        f = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    }

    float out;
    memcpy(&out, &f, 4);
    return out;
}

uint16_t float_to_half(float v) {
    uint32_t f;
    memcpy(&f, &v, 4);

    uint32_t sign = (f >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((f >> 23) & 0xFFu) - 127 + 15;
    uint32_t man  = f & 0x7FFFFFu;

    if (((f >> 23) & 0xFFu) == 0xFFu)             /* inf / nan */
        return (uint16_t)(sign | 0x7C00u | (man ? 0x200u : 0));
    if (exp >= 0x1F)                              /* overflow -> inf */
        return (uint16_t)(sign | 0x7C00u);
    if (exp <= 0) {                               /* subnormal / underflow */
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half_man = man >> shift;
        /* round to nearest */
        if ((man >> (shift - 1)) & 1u) half_man++;
        return (uint16_t)(sign | half_man);
    }
    uint16_t out = (uint16_t)(sign | ((uint32_t)exp << 10) | (man >> 13));
    if (man & 0x1000u) out++;                     /* round to nearest */
    return out;
}

void write_block_rgba8(uint8_t *dst, uint32_t width, uint32_t height,
                       uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                       const uint8_t *block) {
    uint32_t x0 = bx * bw, y0 = by * bh;
    for (uint32_t y = 0; y < bh; y++) {
        uint32_t iy = y0 + y;
        if (iy >= height) break;
        uint32_t copy_w = (x0 + bw <= width) ? bw : (x0 < width ? width - x0 : 0);
        if (copy_w)
            memcpy(dst + ((size_t)iy * width + x0) * 4,
                   block + (size_t)y * bw * 4, (size_t)copy_w * 4);
    }
}

void read_block_rgba8(const uint8_t *src, uint32_t width, uint32_t height,
                      uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                      uint8_t *block) {
    uint32_t x0 = bx * bw, y0 = by * bh;
    for (uint32_t y = 0; y < bh; y++) {
        uint32_t iy = y0 + y;
        if (iy >= height) iy = height - 1;        /* replicate bottom edge */
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t ix = x0 + x;
            if (ix >= width) ix = width - 1;      /* replicate right edge */
            memcpy(block + ((size_t)y * bw + x) * 4,
                   src + ((size_t)iy * width + ix) * 4, 4);
        }
    }
}

void write_block_rgba32f(float *dst, uint32_t width, uint32_t height,
                         uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                         const float *block) {
    uint32_t x0 = bx * bw, y0 = by * bh;
    for (uint32_t y = 0; y < bh; y++) {
        uint32_t iy = y0 + y;
        if (iy >= height) break;
        uint32_t copy_w = (x0 + bw <= width) ? bw : (x0 < width ? width - x0 : 0);
        if (copy_w)
            memcpy(dst + ((size_t)iy * width + x0) * 4,
                   block + (size_t)y * bw * 4, (size_t)copy_w * 16);
    }
}

void read_block_rgba32f(const float *src, uint32_t width, uint32_t height,
                        uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                        float *block) {
    uint32_t x0 = bx * bw, y0 = by * bh;
    for (uint32_t y = 0; y < bh; y++) {
        uint32_t iy = y0 + y;
        if (iy >= height) iy = height - 1;
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t ix = x0 + x;
            if (ix >= width) ix = width - 1;
            memcpy(block + ((size_t)y * bw + x) * 4,
                   src + ((size_t)iy * width + ix) * 4, 16);
        }
    }
}

} /* namespace texc */
