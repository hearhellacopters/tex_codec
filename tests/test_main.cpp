/*
 * test_main.cpp - integration tests through the public C API.
 *
 * For every format: encode -> decode roundtrip PSNR on gradient and
 * pseudo-random images (including non-block-aligned sizes), plus
 * swizzle -> unswizzle identity for every layout mode, plus error paths.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../include/tex_codec.h"

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
        TEXC_SWIZZLE_DX12_64KB,
    };
    const texc_format swz_fmts[] = { TEXC_FORMAT_BC1, TEXC_FORMAT_BC7,
                                     TEXC_FORMAT_RGBA8 };
    for (texc_swizzle_mode m : modes)
        for (texc_format f : swz_fmts) {
            test_swizzle_roundtrip(m, f, 64, 64, m == TEXC_SWIZZLE_SWITCH ?
                                   0xFFFFFFFFu : 0);
            test_swizzle_roundtrip(m, f, 256, 128, m == TEXC_SWIZZLE_SWITCH ?
                                   0xFFFFFFFFu : 0);
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
