/*
 * ps5_oracle - verifies tex_codec's PS5 tiling against Sony's AgcGpuAddress
 * host library, and generates the known-answer table used by tests/.
 *
 * NOT built by default: it needs the PS5 SDK (SIE confidential, never
 * committed). See README.md in this directory for the build line.
 *
 *   ps5_oracle verify              bit-exact matrix vs the DLL (layout +
 *                                  detile of every mip/slice + tile)
 *   ps5_oracle kat                 print tests/ps5_kat.h
 *   ps5_oracle samples <dir>       print sample-file KAT entries for the
 *                                  G1T files in <dir> (decoded via the DLL)
 */
#include <agc_gpu_address.h>
#include "../../include/tex_codec.h"
#include "../../src/unswizzle/ps5_agc.h"
#include "kat_common.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace sce::AgcGpuAddress;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char *fmt, ...)
{
    if (ok) { ++g_pass; return; }
    ++g_fail;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "FAIL: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}

static SurfaceDescription desc_for(uint32_t tm, uint32_t bpe_log2, uint32_t tw,
                                   uint32_t w, uint32_t h, uint32_t mips, uint32_t slices)
{
    SurfaceDescription d{};
    d.m_tileMode = (TileMode)tm;
    d.m_dimension = SurfaceDimension::k2d;
    d.m_bytesPerElementLog2 = bpe_log2;
    d.m_multiElementMultiplier = 1;
    d.m_texelsPerElementWide = tw;
    d.m_texelsPerElementTall = tw;
    d.m_width = w; d.m_height = h; d.m_depth = 1;
    d.m_numFragmentsLog2 = 0;
    d.m_numMips = mips;
    d.m_numSlices = slices;
    d.m_pipeAligned = false;
    return d;
}

static uint32_t log2u(uint32_t v) { uint32_t l = 0; while (v > 1) { v >>= 1; ++l; } return l; }
static uint32_t max_mips(uint32_t w, uint32_t h) { return 1 + log2u(w > h ? w : h); }

/* Compare our internal layout + conversions with the DLL for one case. */
static void verify_case(uint32_t tm, uint32_t eb, uint32_t tw,
                        uint32_t w, uint32_t h, uint32_t mips, uint32_t slices)
{
    const uint32_t bpe_log2 = log2u(eb);
    SurfaceDescription d = desc_for(tm, bpe_log2, tw, w, h, mips, slices);
    SurfaceSummary s;
    Status st = computeSurfaceSummary(&s, &d);
    check(st == kStatusSuccess, "SDK summary tm=%u eb=%u %ux%u mips=%u: %d", tm, eb, w, h, mips, (int)st);
    if (st != kStatusSuccess) return;

    texc_surface_layout lay;
    int rc = texc::ps5::surface_layout(w, h, tw, tw, eb, mips, slices, tm, &lay);
    check(rc == TEXC_OK, "our layout tm=%u eb=%u %ux%u mips=%u rc=%d", tm, eb, w, h, mips, rc);
    if (rc != TEXC_OK) return;

    check(lay.total_size == s.m_totalSizeInBytes, "total tm=%u eb=%u %ux%u m=%u: %llu vs %llu",
          tm, eb, w, h, mips, (unsigned long long)lay.total_size, (unsigned long long)s.m_totalSizeInBytes);
    check(lay.slice_size == s.m_blockSliceSizeInBytes, "slice size tm=%u eb=%u %ux%u m=%u", tm, eb, w, h, mips);
    check(lay.first_mip_in_tail == s.m_firstMipLevelInTail, "first tail tm=%u eb=%u %ux%u m=%u: %u vs %u",
          tm, eb, w, h, mips, lay.first_mip_in_tail, s.m_firstMipLevelInTail);
    check(lay.block_width == s.m_blockWidth && lay.block_height == s.m_blockHeight,
          "block dims tm=%u eb=%u", tm, eb);
    for (uint32_t m = 0; m < mips; ++m) {
        const MipInfo &a = s.m_mips[m];
        const texc_mip_layout &b = lay.mips[m];
        check(a.m_width == b.width && a.m_height == b.height, "mip%u dims tm=%u eb=%u %ux%u: %ux%u vs %ux%u",
              m, tm, eb, w, h, b.width, b.height, a.m_width, a.m_height);
        check(a.m_paddedWidth == b.padded_width && a.m_paddedHeight == b.padded_height,
              "mip%u padded tm=%u eb=%u %ux%u: %ux%u vs %ux%u", m, tm, eb, w, h,
              b.padded_width, b.padded_height, a.m_paddedWidth, a.m_paddedHeight);
        check(a.m_offsetInBytes == b.offset && a.m_sizeInBytes == b.size,
              "mip%u offset/size tm=%u eb=%u %ux%u m=%u: %llu/%llu vs %llu/%llu", m, tm, eb, w, h, mips,
              (unsigned long long)b.offset, (unsigned long long)b.size,
              (unsigned long long)a.m_offsetInBytes, (unsigned long long)a.m_sizeInBytes);
        const bool in_tail = m >= s.m_firstMipLevelInTail;
        check((b.in_tail != 0) == in_tail, "mip%u in_tail", m);
        if (in_tail)
            check(a.m_mipTailCoordX == b.tail_x && a.m_mipTailCoordY == b.tail_y,
                  "mip%u tail coord tm=%u eb=%u %ux%u m=%u: (%u,%u) vs (%u,%u)", m, tm, eb, w, h, mips,
                  b.tail_x, b.tail_y, a.m_mipTailCoordX, a.m_mipTailCoordY);
    }

    /* Random surface: detile every mip / slice both ways. */
    std::vector<uint8_t> surf((size_t)s.m_totalSizeInBytes);
    kat_fill(surf.data(), surf.size(), 0x9E3779B97F4A7C15ull ^ ((uint64_t)tm << 40) ^ ((uint64_t)eb << 32) ^ ((uint64_t)w << 16) ^ h);
    std::vector<uint8_t> rebuilt(surf.size(), 0);
    for (uint32_t sl = 0; sl < slices; ++sl) {
        for (uint32_t m = 0; m < mips; ++m) {
            uint64_t usz = 0;
            computeUntiledSizeForSurface(&usz, &s, m);
            std::vector<uint8_t> ref(usz), ours(usz + 16, 0xAB);
            Status ds = detileSurface(ref.data(), usz, surf.data(), surf.size(), &s, m, sl);
            check(ds == kStatusSuccess, "SDK detile mip%u: %d", m, (int)ds);
            rc = texc::ps5::convert_mip(lay, tm, m, sl, surf.data(), surf.size(), ours.data(), usz, true, false);
            check(rc == TEXC_OK, "our detile mip%u rc=%d", m, rc);
            check(memcmp(ref.data(), ours.data(), usz) == 0,
                  "detile mismatch tm=%u eb=%u %ux%u mips=%u slice=%u mip=%u", tm, eb, w, h, mips, sl, m);
            check(ours[usz] == 0xAB, "detile wrote past linear size");
            /* tile it back with ours into a fresh surface */
            rc = texc::ps5::convert_mip(lay, tm, m, sl, ref.data(), usz, rebuilt.data(), rebuilt.size(), false, false);
            check(rc == TEXC_OK, "our tile mip%u rc=%d", m, rc);
        }
    }
    /* Every element the SDK would read back from `rebuilt` must match. */
    for (uint32_t sl = 0; sl < slices; ++sl)
        for (uint32_t m = 0; m < mips; ++m) {
            uint64_t usz = 0;
            computeUntiledSizeForSurface(&usz, &s, m);
            std::vector<uint8_t> a(usz), b(usz);
            detileSurface(a.data(), usz, surf.data(), surf.size(), &s, m, sl);
            detileSurface(b.data(), usz, rebuilt.data(), rebuilt.size(), &s, m, sl);
            check(memcmp(a.data(), b.data(), usz) == 0, "tile roundtrip mismatch tm=%u eb=%u %ux%u mip=%u", tm, eb, w, h, m);
        }
    /* And our tile must place bytes exactly where the SDK does: tile the
     * SDK-detiled mips with the SDK and compare surfaces byte for byte. */
    std::vector<uint8_t> sdk_built(surf.size(), 0);
    for (uint32_t sl = 0; sl < slices; ++sl)
        for (uint32_t m = 0; m < mips; ++m) {
            uint64_t usz = 0;
            computeUntiledSizeForSurface(&usz, &s, m);
            std::vector<uint8_t> a(usz);
            detileSurface(a.data(), usz, surf.data(), surf.size(), &s, m, sl);
            tileSurface(sdk_built.data(), sdk_built.size(), a.data(), usz, &s, m, sl);
        }
    check(memcmp(sdk_built.data(), rebuilt.data(), surf.size()) == 0,
          "tiled surface bytes differ tm=%u eb=%u %ux%u mips=%u", tm, eb, w, h, mips);
}

static int do_verify()
{
    const uint32_t modes[] = { TEXC_PS5_TILE_STANDARD_256B, TEXC_PS5_TILE_STANDARD_4KB, TEXC_PS5_TILE_STANDARD_64KB };
    const uint32_t ebs[] = { 1, 2, 4, 8, 16 };
    struct Dim { uint32_t w, h; } dims[] = {
        {1,1}, {2,2}, {4,4}, {8,8}, {16,16}, {32,32}, {64,64}, {128,128}, {256,256}, {512,512},
        {256,128}, {128,512}, {512,128}, {1024,64}, {64,1024}, {16,256}, {256,16},
        {37,23}, {23,37}, {100,60}, {300,70}, {70,300}, {129,257}, {1000,1000}, {5,999},
    };
    for (uint32_t tm : modes)
        for (uint32_t eb : ebs)
            for (const Dim &d : dims) {
                const uint32_t mm = max_mips(d.w, d.h);
                verify_case(tm, eb, 1, d.w, d.h, 1, 1);
                verify_case(tm, eb, 1, d.w, d.h, mm, 1);
                if (mm > 2) verify_case(tm, eb, 1, d.w, d.h, mm / 2, 1);
                verify_case(tm, eb, 1, d.w, d.h, mm, 3);
                if (eb >= 8) {
                    /* block-compressed: 4x4 texels per element */
                    verify_case(tm, eb, 4, d.w, d.h, mm, 1);
                    verify_case(tm, eb, 4, d.w, d.h, mm, 6);
                }
            }
    printf("verify: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* Public-API KAT: random surface (kat_fill), detile every mip/slice with
 * the SDK, hash everything. tests/test_main.cpp recomputes with tex_codec. */
static int do_kat()
{
    printf("/* Generated by tools/ps5_oracle (kat) from Sony's AgcGpuAddress host\n"
           " * library. Do not edit by hand. Each entry: tile mode, format, w, h,\n"
           " * mips, slices, surface size, FNV-1a of all mips/slices detiled. */\n"
           "static const ps5_kat k_ps5_kat[] = {\n");
    for (size_t i = 0; i < k_kat_case_count; ++i) {
        const kat_case &c = k_kat_cases[i];
        uint32_t bw, bh, bb;
        texc_block_dims(c.format, &bw, &bh, &bb);
        SurfaceDescription d = desc_for(c.tile_mode, log2u(bb), bw, c.w, c.h, c.mips, c.slices);
        SurfaceSummary s;
        if (computeSurfaceSummary(&s, &d) != kStatusSuccess) { fprintf(stderr, "kat case %zu unsupported\n", i); return 1; }
        std::vector<uint8_t> surf((size_t)s.m_totalSizeInBytes);
        kat_fill(surf.data(), surf.size(), kat_seed(i));
        uint64_t hsh = kat_fnv_init();
        for (uint32_t sl = 0; sl < c.slices; ++sl)
            for (uint32_t m = 0; m < c.mips; ++m) {
                uint64_t usz = 0;
                computeUntiledSizeForSurface(&usz, &s, m);
                std::vector<uint8_t> lin(usz);
                detileSurface(lin.data(), usz, surf.data(), surf.size(), &s, m, sl);
                hsh = kat_fnv(hsh, lin.data(), lin.size());
            }
        printf("    { %u, %s, %u, %u, %u, %u, %lluull, 0x%016llxull },\n", c.tile_mode,
               c.format_name, c.w, c.h, c.mips, c.slices,
               (unsigned long long)s.m_totalSizeInBytes, (unsigned long long)hsh);
    }
    printf("};\n");
    return 0;
}

/* Sample G1T files (never committed): hash of the SDK-detiled mips so the
 * test can verify real content when the samples directory is present. */
static int do_samples(const char *dir)
{
    const char *names[] = { "000cfaa4", "0a8f4727", "0a9fb934", "0x28e2b560", "0x4fff0348",
                            "0x70f1f773", "0xc2e63d48", "0xf773bb73", "0xf7bfb555", "0xfcafccd4",
                            "0xffb7bfa3", "fbff6177" };
    printf("static const ps5_sample_kat k_ps5_samples[] = {\n");
    for (const char *n : names) {
        std::string p = std::string(dir) + "/" + n + ".g1t";
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) { fprintf(stderr, "skip %s\n", p.c_str()); continue; }
        std::vector<uint8_t> b; fseek(f, 0, SEEK_END); b.resize(ftell(f)); fseek(f, 0, SEEK_SET);
        fread(b.data(), 1, b.size(), f); fclose(f);
        const uint8_t *th = b.data() + 0x24;
        uint32_t mips = th[0] >> 4, fmt = th[1], w = 1u << (th[2] & 15), h = 1u << (th[2] >> 4);
        uint32_t ex = *(const uint32_t *)(th + 8);
        size_t off = 0x24 + 8 + ex;
        uint32_t bpe = (fmt == 0x60 || fmt == 0x63) ? 3 : 4;
        const char *fname = fmt == 0x60 ? "TEXC_FORMAT_BC1" : fmt == 0x63 ? "TEXC_FORMAT_BC4" : "TEXC_FORMAT_BC7";
        /* G1T has no tile-mode field: take the standard mode whose surface
         * size equals the data size (what texc_ps5_detect_tile_mode does). */
        const uint32_t tms[] = { TEXC_PS5_TILE_STANDARD_4KB, TEXC_PS5_TILE_STANDARD_64KB,
                                 TEXC_PS5_TILE_STANDARD_256B };
        uint32_t tm = 0;
        SurfaceSummary s;
        for (uint32_t cand : tms) {
            SurfaceDescription d = desc_for(cand, bpe, 4, w, h, mips, 1);
            if (computeSurfaceSummary(&s, &d) == kStatusSuccess &&
                s.m_totalSizeInBytes == b.size() - off) { tm = cand; break; }
        }
        if (!tm) { fprintf(stderr, "%s: no standard tile mode matches %zu bytes\n", n, b.size() - off); continue; }
        uint64_t hsh = kat_fnv_init();
        for (uint32_t m = 0; m < mips; ++m) {
            uint64_t usz = 0; computeUntiledSizeForSurface(&usz, &s, m);
            std::vector<uint8_t> lin(usz);
            detileSurface(lin.data(), usz, b.data() + off, b.size() - off, &s, m, 0);
            hsh = kat_fnv(hsh, lin.data(), lin.size());
        }
        printf("    { \"%s.g1t\", %s, %u, %u, %u, %u, %zu, %zuull, 0x%016llxull },\n", n, fname, w, h, mips,
               tm, off, b.size() - off, (unsigned long long)hsh);
    }
    printf("};\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "verify")) return do_verify();
    if (argc >= 2 && !strcmp(argv[1], "kat")) return do_kat();
    if (argc >= 3 && !strcmp(argv[1], "samples")) return do_samples(argv[2]);
    fprintf(stderr, "usage: ps5_oracle verify | kat | samples <dir>\n");
    return 1;
}
