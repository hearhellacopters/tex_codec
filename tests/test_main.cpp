/*
 * test_main.cpp - integration tests through the public C API.
 *
 * For every format: encode -> decode roundtrip PSNR on gradient and
 * pseudo-random images (including non-block-aligned sizes), plus
 * swizzle -> unswizzle identity for every layout mode, plus error paths.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../include/tex_codec.h"
#include "../tools/ps5_oracle/kat_common.h"
#include "ps5_kat.h"
#include "switch2_kat.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            g_failures++;                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

/* Deterministic xorshift PRNG so runs are reproducible. */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void make_gradient(std::vector<uint8_t> &img, uint32_t w, uint32_t h,
                          bool with_alpha) {
    img.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = &img[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
            p[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            p[2] = (uint8_t)(((x + y) * 127) / (w + h > 2 ? w + h - 2 : 1));
            p[3] = with_alpha ? (uint8_t)(255 - p[0] / 2) : 255;
        }
}

static void make_noise(std::vector<uint8_t> &img, uint32_t w, uint32_t h,
                       bool with_alpha) {
    rng_state = 0x9E3779B9u;
    img.resize((size_t)w * h * 4);
    for (size_t i = 0; i < img.size(); i += 4) {
        /* smooth-ish noise: limited palette so BC-style codecs can cope */
        img[i + 0] = (uint8_t)(rng() % 4 * 80);
        img[i + 1] = (uint8_t)(rng() % 4 * 80);
        img[i + 2] = (uint8_t)(rng() % 4 * 80);
        img[i + 3] = with_alpha ? (uint8_t)(128 + rng() % 128) : 255;
    }
}

/* PSNR over the channels the format actually stores. */
static double psnr(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
                   int channels /* 1,2,3,4 */) {
    double mse = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < channels; c++) {
            double d = (double)a[i + c] - (double)b[i + c];
            mse += d * d;
            n++;
        }
    if (n == 0) return 0;
    mse /= (double)n;
    if (mse <= 1e-9) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

struct fmt_case {
    texc_format fmt;
    int channels;       /* channels compared in PSNR                     */
    bool alpha_input;   /* feed alpha-varying test image                 */
    double min_gradient_psnr;
    bool pow2_only;
};

static const fmt_case k_cases[] = {
    { TEXC_FORMAT_RGBA8,                 4, true,  99.0, false },
    { TEXC_FORMAT_BC1,                   3, false, 28.0, false },
    { TEXC_FORMAT_BC2,                   4, true,  28.0, false },
    { TEXC_FORMAT_BC3,                   4, true,  28.0, false },
    { TEXC_FORMAT_BC4,                   1, false, 32.0, false },
    { TEXC_FORMAT_BC4_SNORM,             1, false, 32.0, false },
    { TEXC_FORMAT_BC5,                   2, false, 32.0, false },
    { TEXC_FORMAT_BC5_SNORM,             2, false, 32.0, false },
    { TEXC_FORMAT_BC6H_UF16,             3, false, 25.0, false },
    { TEXC_FORMAT_BC6H_SF16,             3, false, 25.0, false },
    { TEXC_FORMAT_BC7,                   4, true,  30.0, false },
    { TEXC_FORMAT_ETC1_RGB,              3, false, 26.0, false },
    { TEXC_FORMAT_ETC2_RGB,              3, false, 26.0, false },
    { TEXC_FORMAT_ETC2_RGBA1,            3, false, 26.0, false },
    { TEXC_FORMAT_ETC2_RGBA8,            4, true,  26.0, false },
    { TEXC_FORMAT_EAC_R11,               1, false, 32.0, false },
    { TEXC_FORMAT_EAC_R11_SIGNED,        1, false, 32.0, false },
    { TEXC_FORMAT_EAC_RG11,              2, false, 32.0, false },
    { TEXC_FORMAT_EAC_RG11_SIGNED,       2, false, 32.0, false },
    { TEXC_FORMAT_PVRTC1_2BPP_RGB,       3, false, 18.0, true  },
    { TEXC_FORMAT_PVRTC1_2BPP_RGBA,      3, true,  18.0, true  },
    { TEXC_FORMAT_PVRTC1_4BPP_RGB,       3, false, 22.0, true  },
    { TEXC_FORMAT_PVRTC1_4BPP_RGBA,      3, true,  22.0, true  },
    { TEXC_FORMAT_PVRTC2_2BPP,           3, true,  18.0, false },
    { TEXC_FORMAT_PVRTC2_4BPP,           3, true,  24.0, false },
    { TEXC_FORMAT_ASTC_4x4,              4, true,  24.0, false },
    { TEXC_FORMAT_ASTC_5x4,              4, true,  22.0, false },
    { TEXC_FORMAT_ASTC_5x5,              4, true,  22.0, false },
    { TEXC_FORMAT_ASTC_6x5,              4, true,  21.0, false },
    { TEXC_FORMAT_ASTC_6x6,              4, true,  21.0, false },
    { TEXC_FORMAT_ASTC_8x5,              4, true,  20.0, false },
    { TEXC_FORMAT_ASTC_8x6,              4, true,  20.0, false },
    { TEXC_FORMAT_ASTC_8x8,              4, true,  19.0, false },
    { TEXC_FORMAT_ASTC_10x5,             4, true,  19.0, false },
    { TEXC_FORMAT_ASTC_10x6,             4, true,  19.0, false },
    { TEXC_FORMAT_ASTC_10x8,             4, true,  18.0, false },
    { TEXC_FORMAT_ASTC_10x10,            4, true,  18.0, false },
    { TEXC_FORMAT_ASTC_12x10,            4, true,  17.0, false },
    { TEXC_FORMAT_ASTC_12x12,            4, true,  17.0, false },
    { TEXC_FORMAT_ATC_RGB,               3, false, 28.0, false },
    { TEXC_FORMAT_ATC_RGBA_EXPLICIT,     4, true,  28.0, false },
    { TEXC_FORMAT_ATC_RGBA_INTERPOLATED, 4, true,  28.0, false },
    { TEXC_FORMAT_ETC1_RGB_A_ATLAS,      4, true,  26.0, false },
    { TEXC_FORMAT_PVRTC1_4BPP_RGB_A_ATLAS,4,true,  22.0, true  },
    { TEXC_FORMAT_ETC2_RGB_A_ATLAS,      4, true,  26.0, false },
    { TEXC_FORMAT_PICA_ETC1_RGB8,        3, false, 26.0, false },
    { TEXC_FORMAT_PICA_ETC1_RGB8A4,      4, true,  26.0, false },
    /* Wii colour formats; the intensity ones need a grey input and are
     * covered in test_wii() instead. RGB5A3's alpha is only 3-bit, so it is
     * compared on RGB and its alpha is checked separately there. */
    { TEXC_FORMAT_WII_RGB565,            3, false, 30.0, false },
    { TEXC_FORMAT_WII_RGB5A3,            3, true,  28.0, false },
    { TEXC_FORMAT_WII_RGBA8,             4, true,  60.0, false },
    { TEXC_FORMAT_WII_CMPR,              3, false, 26.0, false },
};

static void test_roundtrip(const fmt_case &fc, uint32_t w, uint32_t h) {
    const char *name = texc_format_name(fc.fmt);
    std::vector<uint8_t> img;
    make_gradient(img, w, h, fc.alpha_input);

    size_t enc_size = texc_encoded_size(fc.fmt, w, h);
    size_t dec_size = texc_decoded_size(w, h);
    CHECK(enc_size > 0, "%s %ux%u: encoded_size == 0", name, w, h);
    if (!enc_size) return;

    std::vector<uint8_t> enc(enc_size, 0xCD);
    std::vector<uint8_t> dec(dec_size, 0);

    int rc = texc_encode(fc.fmt, img.data(), img.size(), w, h,
                         enc.data(), enc.size());
    CHECK(rc == TEXC_OK, "%s %ux%u: encode rc=%d (%s)", name, w, h, rc,
          texc_result_str(rc));
    if (rc != TEXC_OK) return;

    rc = texc_decode(fc.fmt, enc.data(), enc.size(), w, h,
                     dec.data(), dec.size());
    CHECK(rc == TEXC_OK, "%s %ux%u: decode rc=%d (%s)", name, w, h, rc,
          texc_result_str(rc));
    if (rc != TEXC_OK) return;

    double p = psnr(img, dec, fc.channels);
    CHECK(p >= fc.min_gradient_psnr,
          "%s %ux%u: gradient PSNR %.2f dB < %.2f dB", name, w, h, p,
          fc.min_gradient_psnr);
    printf("  %-24s %3ux%-3u gradient %6.2f dB\n", name, w, h, p);

    /* f32 decode path must agree with the 8-bit path (LDR). */
    if (fc.fmt != TEXC_FORMAT_BC6H_UF16 && fc.fmt != TEXC_FORMAT_BC6H_SF16) {
        std::vector<float> decf((size_t)w * h * 4);
        rc = texc_decode_f32(fc.fmt, enc.data(), enc.size(), w, h,
                             decf.data(), decf.size() * 4);
        CHECK(rc == TEXC_OK, "%s: decode_f32 rc=%d", name, rc);
        if (rc == TEXC_OK) {
            int bad = 0;
            for (size_t i = 0; i < decf.size(); i++) {
                int v8 = (int)(decf[i] * 255.0f + 0.5f);
                if (abs(v8 - (int)dec[i]) > 2) bad++;
            }
            CHECK(bad == 0, "%s: decode_f32 disagrees with decode at %d texels",
                  name, bad);
        }
    }
}

static void test_uniform(const fmt_case &fc) {
    /* A solid colour must roundtrip with small error for every codec. */
    const char *name = texc_format_name(fc.fmt);
    uint32_t w = 16, h = 16;
    std::vector<uint8_t> img((size_t)w * h * 4);
    for (size_t i = 0; i < img.size(); i += 4) {
        img[i] = 96; img[i + 1] = 160; img[i + 2] = 224;
        img[i + 3] = fc.alpha_input ? 200 : 255;
    }
    std::vector<uint8_t> enc(texc_encoded_size(fc.fmt, w, h));
    std::vector<uint8_t> dec(texc_decoded_size(w, h));
    if (texc_encode(fc.fmt, img.data(), img.size(), w, h, enc.data(),
                    enc.size()) != TEXC_OK) return;
    if (texc_decode(fc.fmt, enc.data(), enc.size(), w, h, dec.data(),
                    dec.size()) != TEXC_OK) return;
    int worst = 0;
    for (size_t i = 0; i < img.size(); i += 4)
        for (int c = 0; c < fc.channels; c++) {
            int d = abs((int)img[i + c] - (int)dec[i + c]);
            if (d > worst) worst = d;
        }
    /* PVRTC1 atlas: colour and alpha planes bleed across the seam rows
     * (see test_alpha_atlas), so "uniform" is only uniform per-plane. */
    int limit = (fc.fmt == TEXC_FORMAT_PVRTC1_4BPP_RGB_A_ATLAS) ? 32 : 12;
    CHECK(worst <= limit, "%s: uniform colour error %d > %d", name, worst,
          limit);
}

static void test_swizzle_roundtrip(texc_swizzle_mode mode, texc_format fmt,
                                   uint32_t w, uint32_t h, uint32_t arg) {
    size_t linear_size = texc_encoded_size(fmt, w, h);
    size_t tiled_size = texc_swizzled_size(mode, fmt, w, h, arg);
    CHECK(tiled_size >= linear_size,
          "mode %d fmt %s %ux%u: swizzled_size %zu < linear %zu",
          (int)mode, texc_format_name(fmt), w, h, tiled_size, linear_size);
    if (tiled_size < linear_size || linear_size == 0) return;

    std::vector<uint8_t> linear(linear_size);
    rng_state = 0xC0FFEEu;
    for (auto &b : linear) b = (uint8_t)rng();

    std::vector<uint8_t> tiled(tiled_size, 0);
    std::vector<uint8_t> back(linear_size, 0);

    int rc = texc_swizzle(mode, fmt, w, h, linear.data(), linear.size(),
                          tiled.data(), tiled.size(), arg);
    CHECK(rc == TEXC_OK, "mode %d fmt %s %ux%u: swizzle rc=%d", (int)mode,
          texc_format_name(fmt), w, h, rc);
    if (rc != TEXC_OK) return;

    rc = texc_unswizzle(mode, fmt, w, h, tiled.data(), tiled.size(),
                        back.data(), back.size(), arg);
    CHECK(rc == TEXC_OK, "mode %d fmt %s %ux%u: unswizzle rc=%d", (int)mode,
          texc_format_name(fmt), w, h, rc);
    if (rc != TEXC_OK) return;

    CHECK(memcmp(linear.data(), back.data(), linear_size) == 0,
          "mode %d fmt %s %ux%u: swizzle->unswizzle not identity", (int)mode,
          texc_format_name(fmt), w, h);
}

/* Alpha-atlas: the folded alpha must track the source alpha closely, and
 * the encoded size must be exactly double the base codec's.
 *
 * PVRTC1 caveat: its bilinear colour interpolation wraps across the WHOLE
 * double-height image, so the RGB and alpha planes bleed into each other for
 * a few rows at the plane seam (inherent to the KTGL atlas trick, present in
 * engine content too), and the Morton layout means the top half is not a
 * standalone image. So for PVRTC we gate the alpha only on interior rows and
 * skip the plane-independence check; ETC blocks are independent, so both
 * strict checks apply. */
static void test_alpha_atlas(texc_format atlas, texc_format base,
                             bool seam_bleeds) {
    const char *name = texc_format_name(atlas);
    const uint32_t w = 32, h = 32;

    CHECK(texc_encoded_size(atlas, w, h) == 2 * texc_encoded_size(base, w, h),
          "%s: encoded size != 2x base", name);

    std::vector<uint8_t> img;
    make_gradient(img, w, h, false);
    /* Smooth alpha ramp - representable by every base codec. */
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            img[((size_t)y * w + x) * 4 + 3] =
                (uint8_t)(32 + y * 192 / (h - 1));

    std::vector<uint8_t> enc(texc_encoded_size(atlas, w, h));
    std::vector<uint8_t> dec(texc_decoded_size(w, h));
    int rc = texc_encode(atlas, img.data(), img.size(), w, h,
                         enc.data(), enc.size());
    CHECK(rc == TEXC_OK, "%s: encode rc=%d", name, rc);
    rc = texc_decode(atlas, enc.data(), enc.size(), w, h,
                     dec.data(), dec.size());
    CHECK(rc == TEXC_OK, "%s: decode rc=%d", name, rc);
    if (rc != TEXC_OK) return;

    /* PVRTC has TWO seams: the mid-image plane boundary and the vertical
     * wrap-around (bottom of the alpha plane blends with the top of the RGB
     * plane) - exclude the rows adjacent to both. */
    uint32_t y0 = seam_bleeds ? 8 : 0;
    uint32_t y1 = seam_bleeds ? h - 8 : h;
    int worst_a = 0;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = ((size_t)y * w + x) * 4 + 3;
            int d = abs((int)img[i] - (int)dec[i]);
            if (d > worst_a) worst_a = d;
        }
    CHECK(worst_a <= 24, "%s: folded alpha error %d > 24", name, worst_a);

    if (!seam_bleeds) {
        /* Block-independent base: the RGB half decoded as the plain base
         * format must match the atlas decode's RGB exactly. */
        std::vector<uint8_t> base_dec(texc_decoded_size(w, h));
        rc = texc_decode(base, enc.data(), texc_encoded_size(base, w, h),
                         w, h, base_dec.data(), base_dec.size());
        CHECK(rc == TEXC_OK, "%s: base decode rc=%d", name, rc);
        int rgb_mismatch = 0;
        for (size_t i = 0; i < dec.size(); i += 4)
            if (dec[i] != base_dec[i] || dec[i + 1] != base_dec[i + 1] ||
                dec[i + 2] != base_dec[i + 2])
                rgb_mismatch++;
        CHECK(rgb_mismatch == 0,
              "%s: RGB plane differs from base decode at %d px", name,
              rgb_mismatch);
    }
    printf("  %-24s fold max alpha err %d%s\n", name, worst_a,
           seam_bleeds ? " (interior rows; PVRTC seam excluded)" : "");
}

/* alpha_threshold must move the BC1 punchthrough cutoff. */
static void test_encode_options(void) {
    const uint32_t w = 4, h = 4;
    std::vector<uint8_t> img((size_t)w * h * 4);
    for (size_t i = 0; i < img.size(); i += 4) {
        img[i] = 200; img[i + 1] = 100; img[i + 2] = 50;
        img[i + 3] = 100;               /* between the two thresholds */
    }
    std::vector<uint8_t> enc(texc_encoded_size(TEXC_FORMAT_BC1, w, h));
    std::vector<uint8_t> dec(texc_decoded_size(w, h));
    texc_encode_options opts;
    texc_encode_options_init(&opts);
    CHECK(opts.alpha_threshold == 128, "options default threshold != 128");

    opts.alpha_threshold = 50;          /* alpha 100 counts as opaque */
    texc_encode_ex(TEXC_FORMAT_BC1, img.data(), img.size(), w, h,
                   enc.data(), enc.size(), &opts);
    texc_decode(TEXC_FORMAT_BC1, enc.data(), enc.size(), w, h,
                dec.data(), dec.size());
    CHECK(dec[3] == 255, "threshold 50: pixel should be opaque, alpha=%d",
          dec[3]);

    opts.alpha_threshold = 200;         /* alpha 100 counts as transparent */
    texc_encode_ex(TEXC_FORMAT_BC1, img.data(), img.size(), w, h,
                   enc.data(), enc.size(), &opts);
    texc_decode(TEXC_FORMAT_BC1, enc.data(), enc.size(), w, h,
                dec.data(), dec.size());
    CHECK(dec[3] == 0, "threshold 200: pixel should be transparent, alpha=%d",
          dec[3]);
}

static void test_image_utils(void) {
    /* --- profile metadata --- */
    CHECK(texc_profile_bytes_per_pixel(TEXC_PROFILE_RGBA8) == 4, "RGBA8 bpp");
    CHECK(texc_profile_bytes_per_pixel(TEXC_PROFILE_RGB565) == 2, "565 bpp");
    CHECK(texc_profile_bytes_per_pixel(TEXC_PROFILE_RGB10_A2) == 4,
          "RGB10_A2 bpp");
    CHECK(texc_profile_bytes_per_pixel(TEXC_PROFILE_RGBA16F) == 8,
          "RGBA16F bpp");
    CHECK(strcmp(texc_profile_name(TEXC_PROFILE_BGRA8), "BGRA8") == 0,
          "profile name");

    /* --- RGBA8 -> BGRA8 channel swap is exact --- */
    const uint8_t px[8] = { 10, 20, 30, 40, 200, 150, 100, 50 };
    uint8_t out[16];
    int rc = texc_convert_profile(TEXC_PROFILE_RGBA8, px, 8,
                                  TEXC_PROFILE_BGRA8, out, 8, 2);
    CHECK(rc == TEXC_OK && out[0] == 30 && out[1] == 20 && out[2] == 10 &&
          out[3] == 40 && out[4] == 100 && out[7] == 50,
          "RGBA8->BGRA8 swap");

    /* --- RGB8 -> RGBA8: missing alpha becomes opaque --- */
    const uint8_t rgb[3] = { 1, 2, 3 };
    rc = texc_convert_profile(TEXC_PROFILE_RGB8, rgb, 3,
                              TEXC_PROFILE_RGBA8, out, 4, 1);
    CHECK(rc == TEXC_OK && out[0] == 1 && out[1] == 2 && out[2] == 3 &&
          out[3] == 255, "RGB8->RGBA8 default alpha");

    /* --- RGBA8 -> RGB565 -> RGBA8 quantisation stays within 5/6-bit --- */
    uint8_t p565[2], back[4];
    rc = texc_convert_profile(TEXC_PROFILE_RGBA8, px, 4,
                              TEXC_PROFILE_RGB565, p565, 2, 1);
    CHECK(rc == TEXC_OK, "->565 rc");
    rc = texc_convert_profile(TEXC_PROFILE_RGB565, p565, 2,
                              TEXC_PROFILE_RGBA8, back, 4, 1);
    CHECK(rc == TEXC_OK && abs(back[0] - px[0]) <= 8 &&
          abs(back[1] - px[1]) <= 4 && abs(back[2] - px[2]) <= 8 &&
          back[3] == 255, "565 roundtrip within quantisation");

    /* --- RGBA8 -> RGBA16F -> RGBA8 is lossless within a step --- */
    uint8_t h16[8];
    rc = texc_convert_profile(TEXC_PROFILE_RGBA8, px, 4,
                              TEXC_PROFILE_RGBA16F, h16, 8, 1);
    CHECK(rc == TEXC_OK, "->16F rc");
    rc = texc_convert_profile(TEXC_PROFILE_RGBA16F, h16, 8,
                              TEXC_PROFILE_RGBA8, back, 4, 1);
    CHECK(rc == TEXC_OK, "16F-> rc");
    for (int i = 0; i < 4; i++)
        CHECK(abs(back[i] - px[i]) <= 1, "16F roundtrip ch%d: %d vs %d", i,
              back[i], px[i]);

    /* --- pixel_count derivation from src_size --- */
    rc = texc_convert_profile(TEXC_PROFILE_RGBA8, px, 8,
                              TEXC_PROFILE_BGR8, out, 6, 0);
    CHECK(rc == TEXC_OK && out[0] == 30 && out[3] == 100,
          "derived pixel count");

    /* --- flip_y: copy, double-flip identity, in-place --- */
    uint8_t img[2 * 2 * 4];
    for (int i = 0; i < 16; i++) img[i] = (uint8_t)i;
    uint8_t fl[16], fl2[16];
    texc_flip_y(img, 16, fl, 16, 2, 2, 4);
    CHECK(memcmp(fl, img + 8, 8) == 0 && memcmp(fl + 8, img, 8) == 0,
          "flip_y rows swapped");
    texc_flip_y(fl, 16, fl2, 16, 2, 2, 4);
    CHECK(memcmp(fl2, img, 16) == 0, "flip_y twice = identity");
    memcpy(fl2, img, 16);
    texc_flip_y(fl2, 16, fl2, 16, 2, 2, 4);
    CHECK(memcmp(fl2, fl, 16) == 0, "flip_y in place matches copy");

    /* --- flip_x --- */
    texc_flip_x(img, 16, fl, 16, 2, 2, 4);
    CHECK(memcmp(fl, img + 4, 4) == 0 && memcmp(fl + 4, img, 4) == 0 &&
          memcmp(fl + 8, img + 12, 4) == 0, "flip_x pixels mirrored");

    /* --- crop: 2x2 out of 4x4 at (1,1) --- */
    uint8_t big[4 * 4], cropped[2 * 2];
    for (int i = 0; i < 16; i++) big[i] = (uint8_t)i;
    rc = texc_crop(big, 16, 4, 4, 1, 1, 1, 2, 2, cropped, 4);
    CHECK(rc == TEXC_OK && cropped[0] == 5 && cropped[1] == 6 &&
          cropped[2] == 9 && cropped[3] == 10, "crop KAT");
    CHECK(texc_crop(big, 16, 4, 4, 1, 3, 3, 2, 2, cropped, 4) ==
          TEXC_ERR_BAD_DIMENSIONS, "crop out of bounds rejected");

    printf("  image utility checks done\n");
}

/* texc_reswizzle must be byte-identical to texc_swizzle (same function,
 * unambiguous name), and unswizzle(reswizzle(x)) == x. */
static void test_reswizzle_alias(void) {
    const uint32_t w = 64, h = 64;
    size_t lin_size = texc_encoded_size(TEXC_FORMAT_BC7, w, h);
    size_t til_size = texc_swizzled_size(TEXC_SWIZZLE_PS4, TEXC_FORMAT_BC7,
                                         w, h, 0);
    std::vector<uint8_t> lin(lin_size);
    rng_state = 0xBEEF;
    for (auto &b : lin) b = (uint8_t)rng();
    std::vector<uint8_t> t1(til_size, 0), t2(til_size, 1), back(lin_size);

    CHECK(texc_swizzle(TEXC_SWIZZLE_PS4, TEXC_FORMAT_BC7, w, h, lin.data(),
                       lin.size(), t1.data(), t1.size(), 0) == TEXC_OK,
          "swizzle rc");
    CHECK(texc_reswizzle(TEXC_SWIZZLE_PS4, TEXC_FORMAT_BC7, w, h, lin.data(),
                         lin.size(), t2.data(), t2.size(), 0) == TEXC_OK,
          "reswizzle rc");
    CHECK(memcmp(t1.data(), t2.data(), til_size) == 0,
          "reswizzle == swizzle output");
    CHECK(texc_unswizzle(TEXC_SWIZZLE_PS4, TEXC_FORMAT_BC7, w, h, t2.data(),
                         t2.size(), back.data(), back.size(), 0) == TEXC_OK &&
          memcmp(back.data(), lin.data(), lin_size) == 0,
          "unswizzle(reswizzle(x)) == x");
    printf("  reswizzle alias checks done\n");
}

/* PICA200 (3DS) ETC1 container: 8x8 tiles, four 4x4 blocks per tile in
 * (0,0),(4,0),(0,4),(4,4) order, ETC1 block bytes reversed, and for
 * RGB8A4 an 8-byte 4-bit alpha plane in front of each colour block with
 * nibble index x*4 + y. Verified against a standard-ETC1 reference decode
 * so a regression in any one of those rules is caught. */
static void test_pica_etc1(void) {
    /* Sizes: 4bpp / 8bpp, rounded up to whole 8x8 tiles. */
    CHECK(texc_encoded_size(TEXC_FORMAT_PICA_ETC1_RGB8, 64, 64) == 64 * 64 / 2,
          "PICA RGB8 size != 4bpp");
    CHECK(texc_encoded_size(TEXC_FORMAT_PICA_ETC1_RGB8A4, 64, 64) == 64 * 64,
          "PICA RGB8A4 size != 8bpp");
    CHECK(texc_encoded_size(TEXC_FORMAT_PICA_ETC1_RGB8, 1, 1) == 32,
          "PICA size should round up to one 8x8 tile");

    /* A standard ETC1 block (individual mode, flat colour). Decoded as
     * plain ETC1 it gives the reference 4x4; the PICA form is the same
     * eight bytes REVERSED. */
    const uint8_t etc1_be[8] = { 0xF0, 0x50, 0xA0, 0x00, 0, 0, 0, 0 };
    uint8_t ref[4 * 4 * 4];
    CHECK(texc_decode(TEXC_FORMAT_ETC1_RGB, etc1_be, 8, 4, 4, ref,
                      sizeof(ref)) == TEXC_OK, "reference ETC1 decode");

    uint8_t tile[32];
    for (int s = 0; s < 4; s++)
        for (int i = 0; i < 8; i++)
            tile[s * 8 + i] = etc1_be[7 - i];        /* reversed per block */
    uint8_t out[8 * 8 * 4];
    CHECK(texc_decode(TEXC_FORMAT_PICA_ETC1_RGB8, tile, sizeof(tile), 8, 8,
                      out, sizeof(out)) == TEXC_OK, "PICA decode rc");
    bool same = true;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            for (int c = 0; c < 3; c++)
                if (out[(y * 8 + x) * 4 + c] !=
                    ref[((y % 4) * 4 + (x % 4)) * 4 + c])
                    same = false;
    CHECK(same, "PICA byte-reversed ETC1 block != reference ETC1 decode "
                "(got %d,%d,%d want %d,%d,%d)", out[0], out[1], out[2],
          ref[0], ref[1], ref[2]);

    /* Sub-block placement: four distinct colours must land in the right
     * quadrants, i.e. order (0,0), (4,0), (0,4), (4,4). */
    const uint8_t colours[4][3] = {
        { 0xFF, 0x00, 0x00 }, { 0x00, 0xFF, 0x00 },
        { 0x00, 0x00, 0xFF }, { 0xFF, 0xFF, 0x00 } };
    uint8_t t2[32];
    for (int s = 0; s < 4; s++) {
        uint8_t be[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        be[0] = (uint8_t)((colours[s][0] >> 4) * 0x11);
        be[1] = (uint8_t)((colours[s][1] >> 4) * 0x11);
        be[2] = (uint8_t)((colours[s][2] >> 4) * 0x11);
        for (int i = 0; i < 8; i++) t2[s * 8 + i] = be[7 - i];
    }
    CHECK(texc_decode(TEXC_FORMAT_PICA_ETC1_RGB8, t2, sizeof(t2), 8, 8, out,
                      sizeof(out)) == TEXC_OK, "PICA quadrant decode rc");
    const uint32_t qx[4] = { 0, 4, 0, 4 }, qy[4] = { 0, 0, 4, 4 };
    for (int s = 0; s < 4; s++) {
        /* Expected value comes from decoding the SAME bytes as standard
         * ETC1 (index 0 picks a non-zero modifier, so the base colour is
         * not reproduced verbatim) - this isolates the container from the
         * codec, which is what we are testing here. */
        uint8_t be[8], want[4 * 4 * 4];
        for (int i = 0; i < 8; i++) be[i] = t2[s * 8 + 7 - i];
        CHECK(texc_decode(TEXC_FORMAT_ETC1_RGB, be, 8, 4, 4, want,
                          sizeof(want)) == TEXC_OK, "quadrant ref decode");
        const uint8_t *px = &out[((size_t)qy[s] * 8 + qx[s]) * 4];
        CHECK(px[0] == want[0] && px[1] == want[1] && px[2] == want[2],
              "PICA sub-block %d at (%u,%u) = (%d,%d,%d), want (%d,%d,%d)",
              s, qx[s], qy[s], px[0], px[1], px[2],
              want[0], want[1], want[2]);
        /* and the four quadrants must differ from each other */
        if (s > 0) {
            const uint8_t *prev = &out[((size_t)qy[s - 1] * 8 + qx[s - 1]) * 4];
            CHECK(px[0] != prev[0] || px[1] != prev[1] || px[2] != prev[2],
                  "PICA sub-blocks %d and %d decoded identically", s - 1, s);
        }
    }

    /* RGB8A4: alpha plane precedes the colour block, nibble index x*4 + y. */
    uint8_t ta[64];
    memset(ta, 0, sizeof(ta));
    for (int s = 0; s < 4; s++) {
        uint8_t *sp = ta + s * 16;
        for (uint32_t x = 0; x < 4; x++)
            for (uint32_t y = 0; y < 4; y++) {
                uint32_t n = x * 4 + y;
                sp[n >> 1] |= (uint8_t)((n & 0xF) << ((n & 1) * 4));
            }
        for (int i = 0; i < 8; i++) sp[8 + i] = etc1_be[7 - i];
    }
    CHECK(texc_decode(TEXC_FORMAT_PICA_ETC1_RGB8A4, ta, sizeof(ta), 8, 8, out,
                      sizeof(out)) == TEXC_OK, "PICA A4 decode rc");
    bool alpha_ok = true;
    for (uint32_t x = 0; x < 4; x++)
        for (uint32_t y = 0; y < 4; y++) {
            uint8_t want = (uint8_t)(((x * 4 + y) & 0xF) * 0x11);
            if (out[((size_t)y * 8 + x) * 4 + 3] != want) alpha_ok = false;
        }
    CHECK(alpha_ok, "PICA RGB8A4 alpha nibble order/expansion wrong");

    printf("  PICA200 ETC1 container checks done\n");
}

/* PS Vita RAW deswizzle must match DeswizzlePSVitaRaw ->
 * SwizzleMasterFunction(Block32x32Unswizzle) from G1TFormatConvert.h
 * byte-for-byte. The reference mapping is reproduced here independently so
 * a regression in the port is caught rather than compared against itself.
 *
 * G1T drives this path with bitsPerPixel (raw Vita textures are 8/16/24/32
 * bpp - the call site notes 24bpp), so the bytes-per-pixel `arg` override
 * is swept too. */
static void test_vita_raw(void) {
    struct { uint32_t w, h; } sizes[] = {
        { 32, 32 }, { 64, 64 }, { 256, 128 }, { 128, 256 }, { 512, 512 },
        { 16, 16 }, { 48, 32 }, { 37, 23 }, { 96, 64 }, { 33, 33 },
    };
    int mismatches = 0, cases = 0;

    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        const uint32_t w = sizes[si].w, h = sizes[si].h;
        const uint64_t pixels = (uint64_t)w * h;

        for (uint32_t bpp = 1; bpp <= 4; bpp++) {
            const size_t bytes = (size_t)(pixels * bpp);
            std::vector<uint8_t> tiled(bytes);
            for (size_t i = 0; i < bytes; i++)
                tiled[i] = (uint8_t)(i * 7 + (i >> 8));

            /* independent reference */
            std::vector<uint8_t> ref(bytes, 0);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint64_t sw = (uint64_t)(y / 32) * ((uint64_t)w * 32) +
                                  (uint64_t)(y % 32) * 32 +
                                  (uint64_t)(x / 32) * (32 * 32) + (x % 32);
                    if (sw >= pixels) continue;      /* reference skips */
                    uint64_t lin = (uint64_t)y * w + x;
                    memcpy(&ref[(size_t)(lin * bpp)],
                           &tiled[(size_t)(sw * bpp)], bpp);
                }

            /* arg = bytes-per-pixel override; 0 means the format's own 4 */
            const uint32_t arg = (bpp == 4) ? 0 : bpp;
            CHECK(texc_swizzled_size(TEXC_SWIZZLE_PSVITA, TEXC_FORMAT_RGBA8,
                                     w, h, arg) == bytes,
                  "Vita raw %ux%u @%ubpp: swizzled_size != w*h*bpp", w, h, bpp);

            std::vector<uint8_t> mine(bytes, 0xCD);
            int rc = texc_unswizzle(TEXC_SWIZZLE_PSVITA, TEXC_FORMAT_RGBA8,
                                    w, h, tiled.data(), tiled.size(),
                                    mine.data(), mine.size(), arg);
            CHECK(rc == TEXC_OK, "Vita raw %ux%u @%ubpp: rc=%d", w, h, bpp, rc);
            if (rc == TEXC_OK &&
                memcmp(ref.data(), mine.data(), bytes) != 0)
                mismatches++;
            cases++;
        }
    }
    CHECK(mismatches == 0,
          "Vita raw deswizzle differs from the reference in %d/%d cases",
          mismatches, cases);

    /* texc_unswizzled_size must honour the bpp override, otherwise callers
     * that allocate from it (the WASM *_alloc helpers do) get a buffer of
     * the wrong length with a garbage tail. */
    for (uint32_t bpp = 1; bpp <= 4; bpp++) {
        const uint32_t arg = (bpp == 4) ? 0 : bpp;
        CHECK(texc_unswizzled_size(TEXC_SWIZZLE_PSVITA, TEXC_FORMAT_RGBA8,
                                   64, 64, arg) == (size_t)64 * 64 * bpp,
              "unswizzled_size @%ubpp != w*h*bpp", bpp);
    }
    /* every other mode: the linear side is just the format's encoded size */
    CHECK(texc_unswizzled_size(TEXC_SWIZZLE_PS4, TEXC_FORMAT_BC7, 64, 64, 0)
              == texc_encoded_size(TEXC_FORMAT_BC7, 64, 64),
          "unswizzled_size(PS4, BC7) != encoded_size");

    /* On the 32px grid the map is a bijection, so reswizzle restores it. */
    {
        const uint32_t w = 64, h = 64;
        std::vector<uint8_t> lin(w * h * 4), tiled(w * h * 4), back(w * h * 4);
        rng_state = 0x5EED;
        for (auto &b : lin) b = (uint8_t)rng();
        CHECK(texc_reswizzle(TEXC_SWIZZLE_PSVITA, TEXC_FORMAT_RGBA8, w, h,
                             lin.data(), lin.size(), tiled.data(),
                             tiled.size(), 0) == TEXC_OK, "Vita reswizzle rc");
        CHECK(texc_unswizzle(TEXC_SWIZZLE_PSVITA, TEXC_FORMAT_RGBA8, w, h,
                             tiled.data(), tiled.size(), back.data(),
                             back.size(), 0) == TEXC_OK, "Vita unswizzle rc");
        CHECK(memcmp(lin.data(), back.data(), lin.size()) == 0,
              "Vita raw reswizzle->unswizzle not identity on the 32px grid");
    }

    printf("  PS Vita raw: %d cases match the reference\n", cases);
}

/* GameCube/Wii GX formats: known-answer checks derived from tpl.h's rules
 * (big-endian, even column in the high nibble, v*255/max expansion, RGB5A3
 * modes, RGBA8 AR/GB planes, CMPR sub-block order + MSB-first selectors),
 * grey-input roundtrips for the intensity formats, and the palette API. */
static void test_wii(void) {
    /* --- sizes are whole tiles (32B; RGBA8 64B) --- */
    CHECK(texc_encoded_size(TEXC_FORMAT_WII_I4, 8, 8) == 32, "I4 tile 32B");
    /* I8 tiles are 8x4: a 16x8 image is 2x2 tiles = 4 * 32 bytes */
    CHECK(texc_encoded_size(TEXC_FORMAT_WII_I8, 16, 8) == 128, "I8 8x4 tiles");
    CHECK(texc_encoded_size(TEXC_FORMAT_WII_RGBA8, 4, 4) == 64, "RGBA8 tile 64B");
    CHECK(texc_encoded_size(TEXC_FORMAT_WII_CMPR, 8, 8) == 32, "CMPR tile 32B");
    CHECK(texc_encoded_size(TEXC_FORMAT_WII_I4, 1, 1) == 32, "rounds to a tile");

    uint8_t out[8 * 8 * 4];

    /* --- I4: even column = high nibble; expansion v*255/15 --- */
    {
        uint8_t t[32] = { 0 };
        t[0] = 0xF3;                                /* (0,0)=15, (0,1)=3 */
        CHECK(texc_decode(TEXC_FORMAT_WII_I4, t, 32, 8, 8, out, sizeof out) ==
              TEXC_OK, "I4 decode rc");
        CHECK(out[0] == 255 && out[3] == 255, "I4 even column -> high nibble");
        CHECK(out[4] == 3 * 255 / 15, "I4 odd column = %d want %d", out[4],
              3 * 255 / 15);
    }
    /* --- IA4: low nibble intensity, high nibble alpha --- */
    {
        uint8_t t[32] = { 0 };
        t[0] = 0xA5;
        texc_decode(TEXC_FORMAT_WII_IA4, t, 32, 8, 4, out, sizeof out);
        CHECK(out[0] == 5 * 255 / 15 && out[3] == 10 * 255 / 15,
              "IA4 nibbles: got I=%d A=%d", out[0], out[3]);
    }
    /* --- IA8 / RGB565 / RGB5A3: big-endian, both RGB5A3 modes --- */
    {
        uint8_t t[32] = { 0 };
        t[0] = 0x80; t[1] = 0x40;                   /* IA8: A=0x80 I=0x40 */
        texc_decode(TEXC_FORMAT_WII_IA8, t, 32, 4, 4, out, sizeof out);
        CHECK(out[0] == 0x40 && out[3] == 0x80, "IA8 big-endian A|I");

        t[0] = 0xF8; t[1] = 0x00;                   /* RGB565 pure red */
        texc_decode(TEXC_FORMAT_WII_RGB565, t, 32, 4, 4, out, sizeof out);
        CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 255,
              "RGB565 red");

        t[0] = 0xFC; t[1] = 0x00;                   /* RGB5A3 555: r=31 */
        texc_decode(TEXC_FORMAT_WII_RGB5A3, t, 32, 4, 4, out, sizeof out);
        CHECK(out[0] == 255 && out[3] == 255, "RGB5A3 555 mode opaque red");
        t[0] = 0x5F; t[1] = 0x00;                   /* A3=5, r=15 */
        texc_decode(TEXC_FORMAT_WII_RGB5A3, t, 32, 4, 4, out, sizeof out);
        CHECK(out[0] == 255 && out[3] == 5 * 255 / 7,
              "RGB5A3 A3 mode: r=%d a=%d", out[0], out[3]);
    }
    /* --- RGBA8: 32-byte AR plane then 32-byte GB plane --- */
    {
        uint8_t t[64] = { 0 };
        t[0] = 0x11; t[1] = 0x22;                   /* A, R of pixel 0 */
        t[32] = 0x33; t[33] = 0x44;                 /* G, B of pixel 0 */
        texc_decode(TEXC_FORMAT_WII_RGBA8, t, 64, 4, 4, out, sizeof out);
        CHECK(out[0] == 0x22 && out[1] == 0x33 && out[2] == 0x44 &&
              out[3] == 0x11, "RGBA8 AR/GB planes");
    }
    /* --- CMPR: sub-block order TL,TR,BL,BR; BE colours; MSB-first idx --- */
    {
        uint8_t t[32] = { 0 };
        uint8_t *sb = t + 8;                        /* top-right sub-block */
        sb[0] = 0xF8; sb[1] = 0x00;                 /* c0 = red   (BE)   */
        sb[2] = 0x00; sb[3] = 0x1F;                 /* c1 = blue  (BE)   */
        sb[4] = 0x1B;                               /* 00 01 10 11 -> j=0..3 */
        texc_decode(TEXC_FORMAT_WII_CMPR, t, 32, 8, 8, out, sizeof out);
        const uint8_t *row0 = out + 4 * 4;          /* row 0, column 4 */
        CHECK(row0[0] == 255 && row0[2] == 0, "CMPR idx0 = c0 red");
        CHECK(row0[4 + 2] == 255 && row0[4 + 0] == 0, "CMPR idx1 = c1 blue");
        CHECK(row0[8 + 0] == 170 && row0[8 + 2] == 85,
              "CMPR idx2 = (c1+2c0)/3: %d,%d", row0[8], row0[10]);
        CHECK(row0[12 + 0] == 85 && row0[12 + 2] == 170, "CMPR idx3");
        /* untouched TL sub-block: c0 == c1 == 0 is 3-colour mode, but index
         * 0 selects c0 itself, which decodes as opaque black */
        CHECK(out[0] == 0 && out[3] == 255,
              "CMPR zero sub-block idx0 = c0 = opaque black, got a=%d", out[3]);
        /* 3-colour mode: c0 <= c1 makes index 3 transparent black */
        sb[0] = 0x00; sb[1] = 0x1F; sb[2] = 0xF8; sb[3] = 0x00; sb[4] = 0xFF;
        texc_decode(TEXC_FORMAT_WII_CMPR, t, 32, 8, 8, out, sizeof out);
        CHECK(row0[3] == 0 && row0[0] == 0, "CMPR 3-colour idx3 transparent");
    }
    /* --- tiles are row-major: for a 16x8 I8 (2x2 tiles of 8x4), the
     * second tile (bytes 32..63) is pixel column 8, and the third tile
     * (bytes 64..95) is pixel row 4 --- */
    {
        uint8_t t[128] = { 0 }, big[16 * 8 * 4];
        t[32] = 0x77;
        t[64] = 0x99;
        CHECK(texc_decode(TEXC_FORMAT_WII_I8, t, sizeof t, 16, 8, big,
                          sizeof big) == TEXC_OK, "I8 16x8 decode rc");
        CHECK(big[8 * 4] == 0x77 && big[0] == 0, "I8 second tile at x=8");
        CHECK(big[(4 * 16) * 4] == 0x99, "I8 third tile at y=4");
    }

    /* --- intensity roundtrips need grey input (r=g=b) --- */
    {
        const uint32_t w = 64, h = 64;
        std::vector<uint8_t> img((size_t)w * h * 4), enc, dec((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint8_t *p = &img[((size_t)y * w + x) * 4];
                p[0] = p[1] = p[2] = (uint8_t)(x * 255 / (w - 1));
                p[3] = (uint8_t)(y * 255 / (h - 1));
            }
        struct { texc_format f; double min_db; int max_a_err; } cases[] = {
            { TEXC_FORMAT_WII_I4,  30.0, 0 },
            { TEXC_FORMAT_WII_I8,  60.0, 0 },
            { TEXC_FORMAT_WII_IA4, 30.0, 18 },
            { TEXC_FORMAT_WII_IA8, 60.0, 0 },
        };
        for (auto &c : cases) {
            enc.assign(texc_encoded_size(c.f, w, h), 0);
            CHECK(texc_encode(c.f, img.data(), img.size(), w, h, enc.data(),
                              enc.size()) == TEXC_OK, "%s encode",
                  texc_format_name(c.f));
            CHECK(texc_decode(c.f, enc.data(), enc.size(), w, h, dec.data(),
                              dec.size()) == TEXC_OK, "%s decode",
                  texc_format_name(c.f));
            double p = psnr(img, dec, 3);
            CHECK(p >= c.min_db, "%s grey PSNR %.1f < %.1f",
                  texc_format_name(c.f), p, c.min_db);
            bool has_alpha = (c.f == TEXC_FORMAT_WII_IA4 ||
                              c.f == TEXC_FORMAT_WII_IA8);
            int worst = 0;
            for (size_t i = 3; i < img.size(); i += 4) {
                int want = has_alpha ? img[i] : 255;
                int d = abs(want - (int)dec[i]);
                if (d > worst) worst = d;
            }
            CHECK(worst <= c.max_a_err, "%s alpha err %d > %d",
                  texc_format_name(c.f), worst, c.max_a_err);
            printf("  %-24s grey %6.2f dB\n", texc_format_name(c.f), p);
        }
    }

    /* --- paletted: API surface + C4/IA8 known answer --- */
    {
        CHECK(texc_is_paletted(TEXC_FORMAT_WII_C4) &&
              texc_is_paletted(TEXC_FORMAT_WII_C8) &&
              texc_is_paletted(TEXC_FORMAT_WII_C14X2) &&
              !texc_is_paletted(TEXC_FORMAT_WII_CMPR), "is_paletted");
        CHECK(texc_palette_size(TEXC_FORMAT_WII_C4) == 32 &&
              texc_palette_size(TEXC_FORMAT_WII_C8) == 512 &&
              texc_palette_size(TEXC_FORMAT_WII_C14X2) == 32768 &&
              texc_palette_size(TEXC_FORMAT_WII_RGB565) == 0, "palette_size");
        CHECK(texc_can_encode(TEXC_FORMAT_WII_C8) == 0 &&
              texc_can_encode(TEXC_FORMAT_WII_CMPR) == 1, "can_encode");

        uint8_t t[32] = { 0 };
        t[0] = 0x10;                            /* (0,0)=idx 1, (0,1)=idx 0 */
        uint8_t pal[32] = { 0 };
        pal[0] = 0xFF; pal[1] = 0x00;           /* entry 0: IA8 A=FF I=00 */
        pal[2] = 0x80; pal[3] = 0xC0;           /* entry 1: A=80 I=C0     */
        CHECK(texc_decode(TEXC_FORMAT_WII_C4, t, 32, 8, 8, out, sizeof out) ==
              TEXC_ERR_NEEDS_PALETTE, "plain decode of C4 -> NEEDS_PALETTE");
        CHECK(texc_decode_paletted(TEXC_FORMAT_WII_C4, t, 32, 8, 8, pal, 16,
                                   TEXC_PALETTE_IA8, out, sizeof out) ==
              TEXC_ERR_BUFFER_TOO_SMALL, "short palette rejected");
        CHECK(texc_decode_paletted(TEXC_FORMAT_WII_C4, t, 32, 8, 8, pal, 32,
                                   TEXC_PALETTE_IA8, out, sizeof out) ==
              TEXC_OK, "C4 paletted decode rc");
        CHECK(out[0] == 0xC0 && out[3] == 0x80, "C4 (0,0) -> entry 1: I=%d A=%d",
              out[0], out[3]);
        CHECK(out[4] == 0x00 && out[7] == 0xFF, "C4 (0,1) -> entry 0");

        /* RGB5A3 palette entries decode through the same rules */
        pal[2] = 0xFC; pal[3] = 0x00;           /* entry 1: 555 red */
        texc_decode_paletted(TEXC_FORMAT_WII_C4, t, 32, 8, 8, pal, 32,
                             TEXC_PALETTE_RGB5A3, out, sizeof out);
        CHECK(out[0] == 255 && out[1] == 0 && out[3] == 255,
              "C4 with RGB5A3 palette");
        CHECK(texc_encode(TEXC_FORMAT_WII_C4, out, sizeof out, 8, 8, t, 32) ==
              TEXC_ERR_UNSUPPORTED, "paletted encode unsupported");
    }
    printf("  Wii GX format checks done\n");
}

/* PS5 (Agc / GFX10 standard swizzle) mip-chain surfaces against the
 * known-answer table generated from Sony's AgcGpuAddress library
 * (tests/ps5_kat.h), plus reswizzle_mip -> unswizzle_mip identity, the
 * single-mip path, and the real sample files when they are present. */
static_assert(sizeof(texc_mip_layout) == 48, "texc_mip_layout ABI");
static_assert(sizeof(texc_surface_layout) == 48 + 16 * 48,
              "texc_surface_layout ABI (the JS wrapper hard-codes it)");

static uint64_t hash_mips(const texc_surface_layout &lay, texc_swizzle_mode mode,
                          texc_format fmt, uint32_t w, uint32_t h, uint32_t arg,
                          const std::vector<uint8_t> &surf, const char *label,
                          std::vector<uint8_t> *rebuilt) {
    uint64_t hsh = kat_fnv_init();
    for (uint32_t sl = 0; sl < lay.slice_count; ++sl)
        for (uint32_t m = 0; m < lay.mip_count; ++m) {
            const texc_mip_layout &mi = lay.mips[m];
            const size_t lin = (size_t)mi.width * mi.height * lay.element_bytes;
            const uint32_t mw = (w >> m) ? (w >> m) : 1;
            const uint32_t mh = (h >> m) ? (h >> m) : 1;
            CHECK(lin == texc_encoded_size(fmt, mw, mh),
                  "%s mip%u: layout size %zu != encoded size %zu", label, m,
                  lin, texc_encoded_size(fmt, mw, mh));
            std::vector<uint8_t> out(lin + 8, 0xEE);
            int rc = texc_unswizzle_mip(mode, fmt, w, h, lay.mip_count,
                                        lay.slice_count, arg, m, sl,
                                        surf.data(), surf.size(), out.data(), lin);
            CHECK(rc == TEXC_OK, "%s unswizzle_mip mip%u slice%u rc=%d", label,
                  m, sl, rc);
            CHECK(out[lin] == 0xEE, "%s mip%u wrote past the linear size", label, m);
            hsh = kat_fnv(hsh, out.data(), lin);
            if (rebuilt) {
                rc = texc_reswizzle_mip(mode, fmt, w, h, lay.mip_count,
                                        lay.slice_count, arg, m, sl, out.data(),
                                        lin, rebuilt->data(), rebuilt->size());
                CHECK(rc == TEXC_OK, "%s reswizzle_mip mip%u rc=%d", label, m, rc);
            }
        }
    return hsh;
}

static void test_ps5_layout(void) {
    size_t n = 0;
    for (size_t i = 0; i < k_kat_case_count; ++i) {
        const kat_case &c = k_kat_cases[i];
        const ps5_kat &k = k_ps5_kat[i];
        char label[96];
        snprintf(label, sizeof label, "PS5 kat[%zu] tm=%u %s %ux%u m=%u s=%u",
                 i, c.tile_mode, c.format_name, c.w, c.h, c.mips, c.slices);
        CHECK(k.tile_mode == c.tile_mode && k.format == c.format && k.w == c.w &&
              k.h == c.h && k.mips == c.mips && k.slices == c.slices,
              "%s: ps5_kat.h out of sync with kat_common.h", label);

        texc_surface_layout lay;
        int rc = texc_get_surface_layout(TEXC_SWIZZLE_PS5, c.format, c.w, c.h,
                                         c.mips, c.slices, c.tile_mode, &lay);
        CHECK(rc == TEXC_OK, "%s layout rc=%d", label, rc);
        if (rc != TEXC_OK) continue;
        CHECK(lay.total_size == k.surface_size, "%s total %llu != %llu", label,
              (unsigned long long)lay.total_size,
              (unsigned long long)k.surface_size);
        if (lay.total_size != k.surface_size) continue;

        std::vector<uint8_t> surf((size_t)lay.total_size);
        kat_fill(surf.data(), surf.size(), kat_seed(i));
        std::vector<uint8_t> rebuilt(surf.size(), 0);
        const uint64_t hsh = hash_mips(lay, TEXC_SWIZZLE_PS5, c.format, c.w, c.h,
                                       c.tile_mode, surf, label, &rebuilt);
        CHECK(hsh == k.hash, "%s hash %016llx != SDK %016llx", label,
              (unsigned long long)hsh, (unsigned long long)k.hash);
        /* Rebuilding every mip must reproduce the surface (padding bytes are
         * unreachable, so compare through the layout instead). */
        const uint64_t again = hash_mips(lay, TEXC_SWIZZLE_PS5, c.format, c.w,
                                         c.h, c.tile_mode, rebuilt, label, nullptr);
        CHECK(again == hsh, "%s reswizzle_mip -> unswizzle_mip not identity",
              label);

        /* Single-mip API == a one-level surface. */
        texc_surface_layout one;
        rc = texc_get_surface_layout(TEXC_SWIZZLE_PS5, c.format, c.w, c.h, 1, 1,
                                     c.tile_mode, &one);
        CHECK(rc == TEXC_OK && one.total_size ==
                  texc_swizzled_size(TEXC_SWIZZLE_PS5, c.format, c.w, c.h,
                                     c.tile_mode),
              "%s single-mip size mismatch", label);
        ++n;
    }
    printf("  PS5 surface layout: %zu SDK known-answer cases match\n", n);

    /* NONE: mips back to back. */
    {
        texc_surface_layout lay;
        int rc = texc_get_surface_layout(TEXC_SWIZZLE_NONE, TEXC_FORMAT_BC1, 64,
                                         32, 7, 2, 0, &lay);
        CHECK(rc == TEXC_OK && lay.mips[0].offset == 0 &&
                  lay.mips[1].offset == 64 * 32 / 2 &&
                  lay.mips[6].width == 1 && lay.mips[6].height == 1 &&
                  lay.slice_size == 1024 + 256 + 64 + 16 + 8 + 8 + 8 &&
                  lay.total_size == 2 * lay.slice_size,
              "NONE surface layout rc=%d slice_size=%llu", rc,
              (unsigned long long)lay.slice_size);
    }

    /* Tile-mode detection from the data size, on every KAT case. */
    for (size_t i = 0; i < k_kat_case_count; ++i) {
        const kat_case &c = k_kat_cases[i];
        texc_ps5_tile_mode tm = TEXC_PS5_TILE_DEFAULT;
        int rc = texc_ps5_detect_tile_mode(c.format, c.w, c.h, c.mips, c.slices,
                                           k_ps5_kat[i].surface_size, &tm);
        /* 256B and 4KB single-mip sizes can coincide; 4KB wins then. */
        const bool ambiguous = c.tile_mode == TEXC_PS5_TILE_STANDARD_256B &&
                               tm == TEXC_PS5_TILE_STANDARD_4KB;
        CHECK(rc == TEXC_OK && ((uint32_t)tm == c.tile_mode || ambiguous),
              "kat[%zu]: detect_tile_mode -> %d (rc=%d), expected %u", i, (int)tm,
              rc, c.tile_mode);
        CHECK(texc_ps5_detect_tile_mode(c.format, c.w, c.h, c.mips, c.slices,
                                        k_ps5_kat[i].surface_size + 1, &tm) ==
                  TEXC_ERR_BAD_DATA,
              "kat[%zu]: detect_tile_mode accepted a wrong size", i);
    }
    CHECK(texc_ps5_default_tile_mode(TEXC_FORMAT_BC7, 256, 256) ==
                  TEXC_PS5_TILE_STANDARD_4KB &&
              texc_ps5_default_tile_mode(TEXC_FORMAT_BC7, 1024, 128) ==
                  TEXC_PS5_TILE_STANDARD_64KB &&
              texc_ps5_default_tile_mode(TEXC_FORMAT_BC1, 512, 256) ==
                  TEXC_PS5_TILE_STANDARD_4KB,
          "default tile mode heuristic");

    /* Errors. */
    {
        texc_surface_layout lay;
        CHECK(texc_get_surface_layout(TEXC_SWIZZLE_PS5, TEXC_FORMAT_BC7, 64, 64,
                                      8, 1, 0, &lay) == TEXC_ERR_INVALID_ARG,
              "too many mips accepted");
        CHECK(texc_get_surface_layout(TEXC_SWIZZLE_PS5, TEXC_FORMAT_BC7, 64, 64,
                                      1, 1, TEXC_PS5_TILE_LEGACY, &lay) ==
                  TEXC_ERR_UNSUPPORTED,
              "legacy mode has no chain layout");
        CHECK(texc_get_surface_layout(TEXC_SWIZZLE_PS5, TEXC_FORMAT_BC7, 64, 64,
                                      1, 1, 3, &lay) == TEXC_ERR_UNSUPPORTED,
              "unknown PS5 tile mode accepted");
        CHECK(texc_get_surface_layout(TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC7, 64,
                                      64, 1, 1, 0, &lay) == TEXC_ERR_UNSUPPORTED,
              "Switch chain layout unexpectedly supported");
        CHECK(texc_swizzled_size(TEXC_SWIZZLE_PS5, TEXC_FORMAT_BC7, 64, 64, 3) == 0,
              "swizzled_size with bad tile mode");
    }

    /* Real content when available (see tools/ps5_oracle/README.md). */
    size_t found = 0;
    for (const ps5_sample_kat &s : k_ps5_samples) {
        const char *dirs[] = { "samples/ps5/textures/", "../samples/ps5/textures/",
                               "../../samples/ps5/textures/" };
        FILE *f = nullptr;
        for (const char *d : dirs) {
            char path[512];
            snprintf(path, sizeof path, "%s%s", d, s.file);
            f = fopen(path, "rb");
            if (f) break;
        }
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> file(len > 0 ? (size_t)len : 0);
        if (!file.empty() && fread(file.data(), 1, file.size(), f) != file.size())
            file.clear();
        fclose(f);
        if (file.size() < s.data_offset + s.data_size) continue;
        std::vector<uint8_t> surf(file.begin() + s.data_offset,
                                  file.begin() + s.data_offset + (size_t)s.data_size);
        /* G1T has no tile-mode field: recover it from the data size. */
        texc_ps5_tile_mode tm = TEXC_PS5_TILE_DEFAULT;
        int rc = texc_ps5_detect_tile_mode(s.format, s.w, s.h, s.mips, 1,
                                           s.data_size, &tm);
        CHECK(rc == TEXC_OK && (uint32_t)tm == s.tile_mode,
              "%s: detected tile mode %d (rc=%d), SDK says %u", s.file, (int)tm,
              rc, s.tile_mode);
        CHECK(texc_ps5_default_tile_mode(s.format, s.w, s.h) == (texc_ps5_tile_mode)s.tile_mode,
              "%s: default-tile-mode heuristic predicts %d, actual %u", s.file,
              (int)texc_ps5_default_tile_mode(s.format, s.w, s.h), s.tile_mode);
        texc_surface_layout lay;
        rc = texc_get_surface_layout(TEXC_SWIZZLE_PS5, s.format, s.w, s.h,
                                     s.mips, 1, s.tile_mode, &lay);
        CHECK(rc == TEXC_OK && lay.total_size == s.data_size,
              "%s: layout %llu != file data %llu", s.file,
              (unsigned long long)lay.total_size, (unsigned long long)s.data_size);
        if (rc != TEXC_OK || lay.total_size != s.data_size) continue;
        const uint64_t hsh = hash_mips(lay, TEXC_SWIZZLE_PS5, s.format, s.w, s.h,
                                       s.tile_mode, surf, s.file, nullptr);
        CHECK(hsh == s.hash, "%s: hash %016llx != SDK %016llx", s.file,
              (unsigned long long)hsh, (unsigned long long)s.hash);
        ++found;
    }
    if (found)
        printf("  PS5 samples: %zu G1T textures match the SDK detile\n", found);
    else
        printf("  (samples/ps5/textures not found from cwd; skipped)\n");
}

/* Switch vs Switch 2 (Orin): same GOB raster and sizes, two address bits
 * swapped inside each 64-byte x 8-row GOB. One RGBA8 GOB (16x8 pixels,
 * block height 1): pixel (x, y) starts at byte xb = 4x of row y, and its
 * tiled offset is the GOB pattern below (from tegra_swizzle's
 * deswizzle_inner, with and without isOrin). The hash cases were produced
 * by tegra_swizzle_x64.dll's swizzle_block_linear. */
static uint32_t ref_gob(uint32_t xb, uint32_t y, bool orin) {
    return orin ? ((xb % 64) / 32) * 256 + ((y % 8) / 4) * 128 +
                      ((xb % 32) / 16) * 64 + (y % 4) * 16 + (xb % 16)
                : ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
                      ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
}

static void test_switch2(void) {
    for (int pass = 0; pass < 2; ++pass) {
        const bool orin = pass == 1;
        const texc_swizzle_mode m = orin ? TEXC_SWIZZLE_SWITCH2
                                         : TEXC_SWIZZLE_SWITCH;
        CHECK(texc_swizzled_size(m, TEXC_FORMAT_RGBA8, 16, 8, 0) == 512,
              "%s: one GOB", orin ? "SWITCH2" : "SWITCH");
        std::vector<uint8_t> lin(512), tiled(512, 0);
        for (uint32_t i = 0; i < 512; ++i) lin[i] = (uint8_t)(i * 7 + 3);
        int rc = texc_reswizzle(m, TEXC_FORMAT_RGBA8, 16, 8, lin.data(), 512,
                                tiled.data(), 512, 0);
        CHECK(rc == TEXC_OK, "reswizzle rc=%d", rc);
        bool ok = true;
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t xb = 0; xb < 64; ++xb)
                if (tiled[ref_gob(xb, y, orin)] != lin[y * 64 + xb]) ok = false;
        CHECK(ok, "%s GOB byte order", orin ? "SWITCH2" : "SWITCH");
    }
    /* The two modes must agree on sizes everywhere and differ in content. */
    const uint32_t dims[][2] = { {64, 64}, {256, 128}, {37, 23}, {1024, 16} };
    for (auto &d : dims)
        for (uint32_t arg : { 0xFFFFFFFFu, 0u, 3u }) {
            CHECK(texc_swizzled_size(TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC7,
                                     d[0], d[1], arg) ==
                      texc_swizzled_size(TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_BC7,
                                         d[0], d[1], arg),
                  "SWITCH/SWITCH2 sizes differ for %ux%u arg %u", d[0], d[1],
                  arg);
        }
    for (const switch2_kat &k : k_switch2_kat) {
        const size_t lin = texc_encoded_size(k.format, k.w, k.h);
        const size_t ts = texc_swizzled_size(k.mode, k.format, k.w, k.h, k.arg);
        CHECK(ts == k.tiled_size, "switch kat %s %ux%u mode %d: size %zu != %zu",
              texc_format_name(k.format), k.w, k.h, (int)k.mode, ts,
              (size_t)k.tiled_size);
        if (ts != k.tiled_size) continue;
        std::vector<uint8_t> src(lin), tiled(ts, 0);
        kat_fill(src.data(), src.size(), k.seed);
        int rc = texc_reswizzle(k.mode, k.format, k.w, k.h, src.data(), lin,
                                tiled.data(), ts, k.arg);
        const uint64_t h = kat_fnv(kat_fnv_init(), tiled.data(), ts);
        CHECK(rc == TEXC_OK && h == k.hash,
              "switch kat %s %ux%u mode %d arg %u: hash %016llx != DLL %016llx",
              texc_format_name(k.format), k.w, k.h, (int)k.mode, k.arg,
              (unsigned long long)h, (unsigned long long)k.hash);
    }
    printf("  Switch / Switch 2 GOB checks done (%zu DLL cases)\n",
           sizeof(k_switch2_kat) / sizeof(k_switch2_kat[0]));
}

/* D3D12 64KB tiles as KTGL stores them (Rise of the Ronin PC,
 * sub_141E3CD10). The reference below follows that loop: tile t of a mip
 * nx tiles wide sits at (t % nx, t / nx), is min(tw, W - x) x min(th, H - y)
 * pixels, and is read from the file in one piece, row by row, into the
 * linear destination. A mip at most one tile wide is read as plain rows. */
static void ref_dx12_unswizzle(const uint8_t *file, uint8_t *lin, uint32_t w,
                               uint32_t h, uint32_t tw_px, uint32_t th_px,
                               uint32_t bw, uint32_t eb) {
    const uint32_t ew = (w + bw - 1) / bw, eh = (h + bw - 1) / bw;
    const uint32_t nx = (w + tw_px - 1) / tw_px, ny = (h + th_px - 1) / th_px;
    if (nx <= 1) {
        memcpy(lin, file, (size_t)ew * eh * eb);
        return;
    }
    const uint32_t tw = tw_px / bw, th = th_px / bw;
    for (uint32_t t = 0; t < nx * ny; ++t) {
        const uint32_t x0 = t % nx * tw, y0 = t / nx * th;
        const uint32_t cw = ew - x0 < tw ? ew - x0 : tw;
        const uint32_t ch = eh - y0 < th ? eh - y0 : th;
        for (uint32_t r = 0; r < ch; ++r, file += (size_t)cw * eb)
            memcpy(lin + ((size_t)(y0 + r) * ew + x0) * eb, file,
                   (size_t)cw * eb);
    }
}

static void test_dx12_64kb(void) {
    /* tile shapes in pixels, from sub_141E5E6E0 */
    const struct { texc_format f; uint32_t tw, th, bw, eb; } fmts[] = {
        { TEXC_FORMAT_RGBA8, 128, 128, 1, 4 },
        { TEXC_FORMAT_BC1,   512, 256, 4, 8 },
        { TEXC_FORMAT_BC4,   512, 256, 4, 8 },
        { TEXC_FORMAT_BC7,   256, 256, 4, 16 },
    };
    const uint32_t dims[][2] = { {2049, 2049}, {256, 1024}, {129, 2},
                                 {1000, 300}, {64, 64}, {513, 260},
                                 {4096, 8} };
    for (auto &f : fmts)
        for (auto &d : dims) {
            const size_t lin = texc_encoded_size(f.f, d[0], d[1]);
            CHECK(texc_swizzled_size(TEXC_SWIZZLE_DX12_64KB, f.f, d[0], d[1],
                                     0) == lin,
                  "DX12 %s %ux%u: stored size != linear size",
                  texc_format_name(f.f), d[0], d[1]);
            std::vector<uint8_t> file(lin), ref(lin), out(lin);
            kat_fill(file.data(), lin, d[0] * 31 + d[1]);
            ref_dx12_unswizzle(file.data(), ref.data(), d[0], d[1], f.tw,
                               f.th, f.bw, f.eb);
            int rc = texc_unswizzle(TEXC_SWIZZLE_DX12_64KB, f.f, d[0], d[1],
                                    file.data(), lin, out.data(), lin, 0);
            CHECK(rc == TEXC_OK && out == ref, "DX12 %s %ux%u differs from the "
                  "Ronin detile", texc_format_name(f.f), d[0], d[1]);
            test_swizzle_roundtrip(TEXC_SWIZZLE_DX12_64KB, f.f, d[0], d[1], 0);
        }
    /* 129x2 RGBA8: two tiles, the right one 1 pixel wide, after 128x2. */
    std::vector<uint8_t> lin(129 * 2 * 4), til(lin.size());
    for (size_t i = 0; i < lin.size(); ++i) lin[i] = (uint8_t)(i / 4);
    texc_reswizzle(TEXC_SWIZZLE_DX12_64KB, TEXC_FORMAT_RGBA8, 129, 2,
                   lin.data(), lin.size(), til.data(), til.size(), 0);
    CHECK(til[128 * 2 * 4] == 128 && til[128 * 2 * 4 + 4] == (uint8_t)(129 + 128),
          "DX12 129x2 edge tile placement");
    printf("  DX12 64KB tile checks done\n");
}

/* Wii U whole surfaces. The totals are the payload sizes of real Hyrule
 * Warriors G1Ts (rain.g1t, C_ZELDA_1.g1t, GalleryEnvMap.g1t): mip 0 stays
 * 2D_TILED_THIN1, smaller mips fall to 1D_TILED_THIN1, single-mip cubes
 * are six slices. */
static void test_wiiu_layout(void) {
    const struct { texc_format f; uint32_t w, h, mips, slices; uint64_t total; }
        k[] = {
            { TEXC_FORMAT_BC3,   128,  128,  3, 1,   21504 },
            { TEXC_FORMAT_BC1,   128,  128,  6, 1,   12288 },
            { TEXC_FORMAT_BC3,  1024, 1024,  9, 1, 1400832 },
            { TEXC_FORMAT_BC1,  1024, 1024,  9, 1,  700416 },
            { TEXC_FORMAT_BC1,    16,   16,  1, 1,    4096 },
            { TEXC_FORMAT_BC1,   256,  256,  1, 6,  196608 },
            { TEXC_FORMAT_RGBA8, 128,  128,  1, 6,  393216 },
        };
    for (auto &c : k) {
        texc_surface_layout lay;
        int rc = texc_get_surface_layout(TEXC_SWIZZLE_WIIU, c.f, c.w, c.h,
                                         c.mips, c.slices, 0, &lay);
        CHECK(rc == TEXC_OK && lay.total_size == c.total,
              "WIIU %s %ux%u x%u mips x%u slices: total %llu != %llu",
              texc_format_name(c.f), c.w, c.h, c.mips, c.slices,
              (unsigned long long)lay.total_size, (unsigned long long)c.total);
        if (rc != TEXC_OK) continue;
        /* every mip and slice survives reswizzle_mip -> unswizzle_mip */
        std::vector<uint8_t> surf(lay.total_size, 0);
        bool ok = true;
        for (uint32_t s = 0; s < c.slices; ++s)
            for (uint32_t m = 0; m < c.mips; ++m) {
                const size_t n = (size_t)lay.mips[m].width * lay.mips[m].height *
                                 lay.element_bytes;
                std::vector<uint8_t> a(n), b(n);
                kat_fill(a.data(), n, m * 7 + s);
                ok &= texc_reswizzle_mip(TEXC_SWIZZLE_WIIU, c.f, c.w, c.h,
                                         c.mips, c.slices, 0, m, s, a.data(), n,
                                         surf.data(), surf.size()) == TEXC_OK;
                ok &= texc_unswizzle_mip(TEXC_SWIZZLE_WIIU, c.f, c.w, c.h,
                                         c.mips, c.slices, 0, m, s, surf.data(),
                                         surf.size(), b.data(), n) == TEXC_OK &&
                      a == b;
            }
        CHECK(ok, "WIIU %s %ux%u: mip roundtrip", texc_format_name(c.f), c.w, c.h);
    }
    /* Mip 1 of 128x128 BC3 (16x16 blocks) is 1D_TILED_THIN1 right after
     * mip 0: 8x8-block micro tiles, row by row at a pitch of 16 blocks,
     * 128-bit order y0 x0 x1 x2 y1 y2 inside a tile. */
    texc_surface_layout lay;
    texc_get_surface_layout(TEXC_SWIZZLE_WIIU, TEXC_FORMAT_BC3, 128, 128, 3, 1,
                            0, &lay);
    CHECK(lay.mips[1].offset == 16384 && lay.mips[2].offset == 20480,
          "WIIU 128x128 BC3 mip offsets");
    std::vector<uint8_t> lin(16 * 16 * 16), surf(lay.total_size, 0);
    for (size_t i = 0; i < lin.size(); ++i) lin[i] = (uint8_t)(i / 16);
    texc_reswizzle_mip(TEXC_SWIZZLE_WIIU, TEXC_FORMAT_BC3, 128, 128, 3, 1, 0, 1,
                       0, lin.data(), lin.size(), surf.data(), surf.size());
    bool ok = true;
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x) {
            const uint32_t idx = (y & 1) | (x & 7) << 1 | (y & 6) << 3;
            const size_t off = 16384 + ((y / 8) * 2 + x / 8) * 1024 + idx * 16;
            if (surf[off] != (uint8_t)(y * 16 + x)) ok = false;
        }
    CHECK(ok, "WIIU 1D mip element order");
    /* GX2 keeps a level's slices together, which the slice-major layout
     * struct cannot describe */
    CHECK(texc_get_surface_layout(TEXC_SWIZZLE_WIIU, TEXC_FORMAT_BC1, 64, 64, 2,
                                  6, 0, &lay) == TEXC_ERR_UNSUPPORTED,
          "WIIU mips x slices rejected");
    printf("  Wii U surface layout checks done\n");
}

/* X360: byte order per element size (Dynasty Warriors 8's G1T format ->
 * D3DFORMAT table) and the loader's stored-rows form. */
static std::vector<uint8_t> x360_tile(texc_format f, uint32_t w, uint32_t h,
                                      const std::vector<uint8_t> &lin,
                                      uint32_t arg) {
    std::vector<uint8_t> t(texc_swizzled_size(TEXC_SWIZZLE_X360, f, w, h, arg));
    int rc = texc_reswizzle(TEXC_SWIZZLE_X360, f, w, h, lin.data(), lin.size(),
                            t.data(), t.size(), arg);
    CHECK(rc == TEXC_OK && !t.empty(), "X360 reswizzle %s %ux%u arg %x rc=%d",
          texc_format_name(f), w, h, arg, rc);
    return t;
}

static void test_x360(void) {
    /* AUTO = 8in32 for 4-byte texels, 8in16 for blocks; 8in32 is NONE with
     * each 32-bit word reversed. */
    {
        std::vector<uint8_t> lin(64 * 32 * 4);
        kat_fill(lin.data(), lin.size(), 360);
        auto none = x360_tile(TEXC_FORMAT_RGBA8, 64, 32, lin, TEXC_X360_ENDIAN_NONE);
        auto w32 = x360_tile(TEXC_FORMAT_RGBA8, 64, 32, lin, TEXC_X360_ENDIAN_8IN32);
        auto aut = x360_tile(TEXC_FORMAT_RGBA8, 64, 32, lin, 0);
        auto h32 = x360_tile(TEXC_FORMAT_RGBA8, 64, 32, lin, TEXC_X360_ENDIAN_16IN32);
        bool ok = true, ok16 = true;
        for (size_t i = 0; i < none.size(); ++i) {
            if (w32[i] != none[(i & ~3u) + 3 - (i & 3)]) ok = false;
            if (h32[i] != none[(i & ~3u) + ((i & 3) ^ 2)]) ok16 = false;
        }
        CHECK(ok, "X360 8IN32 is NONE with 32-bit words reversed");
        CHECK(ok16, "X360 16IN32 is NONE with 16-bit halves swapped");
        CHECK(aut == w32, "X360 AUTO for RGBA8 is 8IN32");
    }
    {
        std::vector<uint8_t> lin(texc_encoded_size(TEXC_FORMAT_BC1, 128, 128));
        kat_fill(lin.data(), lin.size(), 361);
        CHECK(x360_tile(TEXC_FORMAT_BC1, 128, 128, lin, 0) ==
                  x360_tile(TEXC_FORMAT_BC1, 128, 128, lin, TEXC_X360_ENDIAN_8IN16),
              "X360 AUTO for BC1 is 8IN16");
    }
    CHECK(texc_swizzled_size(TEXC_SWIZZLE_X360, TEXC_FORMAT_RGBA8, 64, 64,
                             2 | TEXC_X360_ENDIAN_8IN32) == 0,
          "X360 8IN32 with a 2-byte pitch rejected");
    for (uint32_t e : { 0x000u, 0x100u, 0x200u, 0x300u, 0x400u })
        test_swizzle_roundtrip(TEXC_SWIZZLE_X360, TEXC_FORMAT_RGBA8, 64, 64, e);

    /* LOADER_ROWS: stored row r = the tiled surface's bytes at r * pitch,
     * one row of elements long. */
    const struct { texc_format f; uint32_t w, h; } cases[] = {
        { TEXC_FORMAT_RGBA8, 256, 128 },   /* row == pitch       */
        { TEXC_FORMAT_RGBA8, 40, 8 },      /* row < pitch        */
        { TEXC_FORMAT_BC1, 128, 64 },      /* the request's case */
        { TEXC_FORMAT_BC3, 200, 100 },
    };
    for (auto &c : cases) {
        const uint32_t bw = c.f == TEXC_FORMAT_RGBA8 ? 1 : 4;
        const uint32_t gw = (c.w + bw - 1) / bw, gh = (c.h + bw - 1) / bw;
        const size_t lin_size = texc_encoded_size(c.f, c.w, c.h);
        const size_t eb = lin_size / ((size_t)gw * gh);
        const size_t row = gw * eb, pitch = ((gw + 31) & ~31u) * eb;
        CHECK(texc_swizzled_size(TEXC_SWIZZLE_X360, c.f, c.w, c.h,
                                 TEXC_X360_LOADER_ROWS) == lin_size,
              "X360 rows %ux%u: stored size != linear size", c.w, c.h);
        std::vector<uint8_t> lin(lin_size);
        kat_fill(lin.data(), lin_size, c.w + c.h);
        auto surf = x360_tile(c.f, c.w, c.h, lin, 0);
        auto rows = x360_tile(c.f, c.w, c.h, lin, TEXC_X360_LOADER_ROWS);
        std::vector<uint8_t> want(lin_size), loaded(surf.size(), 0);
        for (uint32_t r = 0; r < gh; ++r) {
            memcpy(&want[r * row], &surf[r * pitch], row);
            memcpy(&loaded[r * pitch], &surf[r * pitch], row);
        }
        CHECK(rows == want, "X360 rows %s %ux%u: stored rows != surface rows",
              texc_format_name(c.f), c.w, c.h);
        /* unswizzle(rows) = unswizzle of the surface the loader fills */
        std::vector<uint8_t> a(lin_size), b(lin_size);
        texc_unswizzle(TEXC_SWIZZLE_X360, c.f, c.w, c.h, loaded.data(),
                       loaded.size(), a.data(), lin_size, 0);
        int rc = texc_unswizzle(TEXC_SWIZZLE_X360, c.f, c.w, c.h, rows.data(),
                                rows.size(), b.data(), lin_size,
                                TEXC_X360_LOADER_ROWS);
        CHECK(rc == TEXC_OK && a == b, "X360 rows %s %ux%u: unswizzle differs "
              "from the loaded surface", texc_format_name(c.f), c.w, c.h);
        /* short input: the missing tail reads as zeros */
        const size_t half = lin_size / 2;
        std::fill(loaded.begin(), loaded.end(), 0);
        for (size_t i = 0; i < half; ++i)
            loaded[i / row * pitch + i % row] = rows[i];
        texc_unswizzle(TEXC_SWIZZLE_X360, c.f, c.w, c.h, loaded.data(),
                       loaded.size(), a.data(), lin_size, 0);
        rc = texc_unswizzle(TEXC_SWIZZLE_X360, c.f, c.w, c.h, rows.data(),
                            half, b.data(), lin_size, TEXC_X360_LOADER_ROWS);
        CHECK(rc == TEXC_OK && a == b, "X360 rows %s %ux%u: short input",
              texc_format_name(c.f), c.w, c.h);
    }
    /* full-width, 32-aligned: nothing is lost, so rows roundtrip */
    test_swizzle_roundtrip(TEXC_SWIZZLE_X360, TEXC_FORMAT_BC1, 128, 128,
                           TEXC_X360_LOADER_ROWS);
    printf("  X360 byte order + loader rows checks done\n");
}

static void test_error_paths(void) {
    uint8_t buf[64] = {0};
    CHECK(texc_decode(TEXC_FORMAT_BC1, nullptr, 0, 4, 4, buf, 64) ==
              TEXC_ERR_INVALID_ARG, "null src not rejected");
    CHECK(texc_decode(TEXC_FORMAT_BC1, buf, 2, 4, 4, buf, 64) ==
              TEXC_ERR_BUFFER_TOO_SMALL, "short src not rejected");
    CHECK(texc_decode((texc_format)999, buf, 64, 4, 4, buf, 64) ==
              TEXC_ERR_INVALID_ARG, "bad format not rejected");
    CHECK(texc_encoded_size(TEXC_FORMAT_BC1, 0, 4) == 0, "zero dim size != 0");
    CHECK(texc_format_name(TEXC_FORMAT_BC7) != nullptr, "BC7 name missing");
    CHECK(texc_version() >= (1u << 16), "version");
}

/* The version lives in include/tex_codec.h and is mirrored by CMake (build
 * + .rc resource) and by the JS wrapper. Catch drift here rather than in a
 * shipped artifact. */
static void test_version(void) {
    CHECK(texc_version() == TEXC_VERSION_NUMBER,
          "library reports %06x but header declares %06x", texc_version(),
          TEXC_VERSION_NUMBER);
    CHECK(strcmp(texc_version_string(), TEXC_VERSION_STRING) == 0,
          "version string '%s' != header '%s'", texc_version_string(),
          TEXC_VERSION_STRING);
    CHECK(TEXC_VERSION_NUMBER ==
              TEXC_VERSION_ENCODE(TEXC_VERSION_MAJOR, TEXC_VERSION_MINOR,
                                  TEXC_VERSION_PATCH),
          "TEXC_VERSION_NUMBER encoding");

    const char *info = texc_build_info();
    CHECK(info && strstr(info, TEXC_VERSION_STRING) != nullptr,
          "build info '%s' does not carry the version", info ? info : "(null)");
    printf("  %s\n", info);

    /* The JS wrapper hard-codes the version; keep it honest. */
    FILE *f = fopen("wasm/tex_codec_api.mjs", "rb");
    if (!f) f = fopen("../wasm/tex_codec_api.mjs", "rb");
    if (!f) f = fopen("../../wasm/tex_codec_api.mjs", "rb");
    if (f) {
        char line[512];
        bool found = false;
        char want[64];
        snprintf(want, sizeof(want), "export const VERSION = \"%s\";",
                 TEXC_VERSION_STRING);
        while (fgets(line, sizeof(line), f))
            if (strstr(line, "export const VERSION")) {
                found = strstr(line, want) != nullptr;
                if (!found)
                    printf("    wrapper line: %s", line);
                break;
            }
        fclose(f);
        CHECK(found, "wasm/tex_codec_api.mjs VERSION != %s",
              TEXC_VERSION_STRING);
    } else {
        printf("    (wrapper not found from cwd; skipped JS version check)\n");
    }
}

int main(void) {
    printf("tex_codec test suite (version %s)\n\n", texc_version_string());

    printf("[1/6] encode->decode roundtrips\n");
    for (const fmt_case &fc : k_cases) {
        test_roundtrip(fc, 64, 64);
        if (!fc.pow2_only)
            test_roundtrip(fc, 37, 23);   /* non-block-aligned */
        test_uniform(fc);
    }

    printf("\n[2/6] reswizzle/unswizzle identity\n");
    const texc_swizzle_mode modes[] = {
        TEXC_SWIZZLE_NONE, TEXC_SWIZZLE_PS4, TEXC_SWIZZLE_PS5,
        TEXC_SWIZZLE_SWITCH, TEXC_SWIZZLE_PSVITA, TEXC_SWIZZLE_X360,
        TEXC_SWIZZLE_PSP, TEXC_SWIZZLE_3DS, TEXC_SWIZZLE_WIIU,
        TEXC_SWIZZLE_DX12_64KB, TEXC_SWIZZLE_SWITCH2,
    };
    const texc_format swz_fmts[] = { TEXC_FORMAT_BC1, TEXC_FORMAT_BC7,
                                     TEXC_FORMAT_RGBA8 };
    for (texc_swizzle_mode m : modes)
        for (texc_format f : swz_fmts) {
            const bool gob = m == TEXC_SWIZZLE_SWITCH ||
                             m == TEXC_SWIZZLE_SWITCH2;
            test_swizzle_roundtrip(m, f, 64, 64, gob ? 0xFFFFFFFFu : 0);
            test_swizzle_roundtrip(m, f, 256, 128, gob ? 0xFFFFFFFFu : 0);
            if (gob)   /* odd sizes: GOB modes pad, so they stay invertible */
                test_swizzle_roundtrip(m, f, 37, 23, 0xFFFFFFFFu);
        }
    printf("  swizzle identity checks done\n");

    /* atlas formats swizzle as the base codec at double height */
    test_swizzle_roundtrip(TEXC_SWIZZLE_PS4, TEXC_FORMAT_ETC1_RGB_A_ATLAS,
                           64, 64, 0);
    test_swizzle_roundtrip(TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_ETC2_RGB_A_ATLAS,
                           64, 64, 0xFFFFFFFFu);

    printf("\n[3/6] alpha atlas + encode options\n");
    test_alpha_atlas(TEXC_FORMAT_ETC1_RGB_A_ATLAS, TEXC_FORMAT_ETC1_RGB,
                     false);
    test_alpha_atlas(TEXC_FORMAT_PVRTC1_4BPP_RGB_A_ATLAS,
                     TEXC_FORMAT_PVRTC1_4BPP_RGB, true);
    test_alpha_atlas(TEXC_FORMAT_ETC2_RGB_A_ATLAS, TEXC_FORMAT_ETC2_RGB,
                     false);
    test_encode_options();

    printf("\n[4/6] image utilities + reswizzle\n");
    test_image_utils();
    test_reswizzle_alias();

    test_pica_etc1();
    test_vita_raw();
    test_wii();
    test_ps5_layout();
    test_switch2();
    test_dx12_64kb();
    test_x360();
    test_wiiu_layout();
    /* the pre-1.6 PS5 layout stays reachable and invertible */
    test_swizzle_roundtrip(TEXC_SWIZZLE_PS5, TEXC_FORMAT_BC7, 256, 128,
                           TEXC_PS5_TILE_LEGACY);
    test_swizzle_roundtrip(TEXC_SWIZZLE_PS5, TEXC_FORMAT_BC1, 64, 64,
                           TEXC_PS5_TILE_STANDARD_64KB);
    test_swizzle_roundtrip(TEXC_SWIZZLE_PS5, TEXC_FORMAT_RGBA8, 37, 23,
                           TEXC_PS5_TILE_STANDARD_256B);

    printf("\n[5/6] version + error paths\n");
    test_version();
    test_error_paths();
    printf("  error path checks done\n");

    printf("\n[6/6] noise-image smoke test (no PSNR gate, must not crash)\n");
    for (const fmt_case &fc : k_cases) {
        if (fc.pow2_only) continue;
        std::vector<uint8_t> img;
        make_noise(img, 20, 20, fc.alpha_input);
        std::vector<uint8_t> enc(texc_encoded_size(fc.fmt, 20, 20));
        std::vector<uint8_t> dec(texc_decoded_size(20, 20));
        texc_encode(fc.fmt, img.data(), img.size(), 20, 20, enc.data(),
                    enc.size());
        texc_decode(fc.fmt, enc.data(), enc.size(), 20, 20, dec.data(),
                    dec.size());
    }
    printf("  smoke test done\n");

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL TESTS PASSED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
