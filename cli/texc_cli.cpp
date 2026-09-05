/*
 * texc_cli.cpp — command line front end for tex_codec.
 *
 *   texc decode    -i data.bin -f BC7 -w 256 -h 256 -o out.png
 *   texc decode    -i file.g1t --offset 0x1234 -f ETC1_RGB_A_ATLAS
 *                  -w 128 -h 128 --swizzle switch --arg auto -o out.png
 *   texc encode    -i in.tga -f ETC2_RGBA8 -o out.bin
 *   texc unswizzle -i tiled.bin -f BC1 -w 256 -h 256 -m ps4 -o linear.bin
 *   texc reswizzle -i linear.bin -f BC1 -w 256 -h 256 -m ps4 -o tiled.bin
 *   texc convert   -i in.raw --src-profile RGBA8 --dst-profile BGRA8 -o out.raw
 *   texc flip      -i in.raw -w 64 -h 64 --bpp 4 --dir y -o out.raw
 *   texc crop      -i in.raw -w 64 -h 64 --bpp 4 --rect 8,8,32,32 -o out.raw
 *   texc info      -f ASTC_8x8 -w 256 -h 256 [-m switch --arg auto]
 *   texc formats | texc modes | texc profiles
 *
 * Run `texc help <command>` for the full option list of each command.
 */

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../include/tex_codec.h"

/* ------------------------------------------------------------- utilities */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "texc: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void die_rc(const char *what, int rc) {
    die("%s failed: %s (code %d)", what, texc_result_str(rc), rc);
}

static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static uint64_t parse_u64(const char *s, const char *what) {
    char *end = nullptr;
    uint64_t v = strtoull(s, &end, 0);      /* accepts decimal and 0x hex */
    if (!end || *end) die("invalid %s: '%s'", what, s);
    return v;
}

/* --------------------------------------------------------------- file io */

static std::vector<uint8_t> read_file(const std::string &path,
                                      uint64_t offset, uint64_t size) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) die("cannot open input '%s'", path.c_str());
    fseek(f, 0, SEEK_END);
    uint64_t file_size = (uint64_t)ftell(f);
    if (offset > file_size)
        die("--offset %llu is past the end of '%s' (%llu bytes)",
            (unsigned long long)offset, path.c_str(),
            (unsigned long long)file_size);
    if (size == 0) size = file_size - offset;
    if (offset + size > file_size)
        die("--offset+--size exceeds '%s' (%llu bytes)", path.c_str(),
            (unsigned long long)file_size);
    std::vector<uint8_t> data((size_t)size);
    fseek(f, (long)offset, SEEK_SET);
    if (size && fread(data.data(), 1, (size_t)size, f) != (size_t)size)
        die("short read from '%s'", path.c_str());
    fclose(f);
    return data;
}

static void write_file(const std::string &path,
                       const void *data, size_t size) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) die("cannot open output '%s'", path.c_str());
    if (size && fwrite(data, 1, size, f) != size)
        die("short write to '%s'", path.c_str());
    fclose(f);
    printf("wrote %s (%zu bytes)\n", path.c_str(), size);
}

static std::string ext_of(const std::string &path) {
    size_t dot = path.find_last_of('.');
    size_t sep = path.find_last_of("/\\");
    if (dot == std::string::npos ||
        (sep != std::string::npos && dot < sep)) return "";
    return lower(path.substr(dot + 1));
}

/* -------------------------------------------------------- PNG (stored) */
/* Minimal dependency-free PNG writer: valid zlib stream made of stored
 * (uncompressed) deflate blocks. Bigger files than a real encoder, but
 * readable by every viewer. */

static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put_u32be(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

static void png_chunk(std::vector<uint8_t> &out, const char type[4],
                      const std::vector<uint8_t> &data) {
    put_u32be(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_u32be(out, crc32_of(out.data() + start, out.size() - start));
}

static void write_png(const std::string &path, const uint8_t *rgba,
                      uint32_t w, uint32_t h) {
    std::vector<uint8_t> out;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    put_u32be(ihdr, w); put_u32be(ihdr, h);
    ihdr.push_back(8);  /* bit depth */
    ihdr.push_back(6);  /* colour type RGBA */
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    png_chunk(out, "IHDR", ihdr);

    /* raw scanlines: filter byte 0 + row */
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * ((size_t)w * 4 + 1));
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + (size_t)y * w * 4,
                   rgba + (size_t)(y + 1) * w * 4);
    }

    /* zlib: header + stored deflate blocks + adler32 */
    std::vector<uint8_t> idat;
    idat.push_back(0x78); idat.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        size_t chunk = raw.size() - pos;
        if (chunk > 65535) chunk = 65535;
        bool final_block = (pos + chunk == raw.size());
        idat.push_back(final_block ? 1 : 0);
        idat.push_back((uint8_t)(chunk & 0xFF));
        idat.push_back((uint8_t)(chunk >> 8));
        idat.push_back((uint8_t)(~chunk & 0xFF));
        idat.push_back((uint8_t)(~chunk >> 8));
        idat.insert(idat.end(), raw.begin() + pos, raw.begin() + pos + chunk);
        pos += chunk;
        if (final_block) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put_u32be(idat, (b << 16) | a);
    png_chunk(out, "IDAT", idat);
    png_chunk(out, "IEND", {});
    write_file(path, out.data(), out.size());
}

/* ------------------------------------------------------------------ TGA */

static void write_tga(const std::string &path, const uint8_t *rgba,
                      uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(18, 0);
    out[2] = 2;                              /* uncompressed truecolour */
    out[12] = (uint8_t)w;  out[13] = (uint8_t)(w >> 8);
    out[14] = (uint8_t)h;  out[15] = (uint8_t)(h >> 8);
    out[16] = 32;
    out[17] = 0x28;                          /* top-left origin, 8 alpha */
    out.reserve(18 + (size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {  /* RGBA -> BGRA */
        out.push_back(rgba[i * 4 + 2]);
        out.push_back(rgba[i * 4 + 1]);
        out.push_back(rgba[i * 4 + 0]);
        out.push_back(rgba[i * 4 + 3]);
    }
    write_file(path, out.data(), out.size());
}

/* Uncompressed 24/32-bit TGA -> RGBA8. Returns dimensions. */
static std::vector<uint8_t> read_tga(const std::vector<uint8_t> &tga,
                                     uint32_t *w_out, uint32_t *h_out) {
    if (tga.size() < 18) die("TGA too small");
    uint8_t idlen = tga[0], cmap = tga[1], type = tga[2];
    if (cmap != 0 || type != 2)
        die("only uncompressed truecolour TGA (type 2) is supported");
    uint32_t w = tga[12] | (tga[13] << 8);
    uint32_t h = tga[14] | (tga[15] << 8);
    uint8_t bpp = tga[16];
    bool top_left = (tga[17] & 0x20) != 0;
    if (bpp != 24 && bpp != 32) die("TGA must be 24 or 32 bpp");
    size_t px_off = 18 + idlen;
    size_t px_bytes = (size_t)w * h * (bpp / 8);
    if (tga.size() < px_off + px_bytes) die("TGA truncated");

    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t src_y = top_left ? y : (h - 1 - y);
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = tga.data() + px_off +
                ((size_t)src_y * w + x) * (bpp / 8);
            uint8_t *d = &rgba[((size_t)y * w + x) * 4];
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
            d[3] = (bpp == 32) ? p[3] : 255;
        }
    }
    *w_out = w;
    *h_out = h;
    return rgba;
}

/* -------------------------------------------------------- name look-ups */

static texc_format parse_format(const std::string &name) {
    std::string want = lower(name);
    for (int f = 1; f < TEXC_FORMAT_COUNT; f++) {
        const char *n = texc_format_name((texc_format)f);
        if (n && lower(n) == want) return (texc_format)f;
    }
    die("unknown format '%s' (run `texc formats` for the list)",
        name.c_str());
    return TEXC_FORMAT_INVALID;
}

struct mode_name { const char *name; texc_swizzle_mode mode; };
static const mode_name k_modes[] = {
    { "none",      TEXC_SWIZZLE_NONE },
    { "ps4",       TEXC_SWIZZLE_PS4 },
    { "ps5",       TEXC_SWIZZLE_PS5 },
    { "switch",    TEXC_SWIZZLE_SWITCH },
    { "psvita",    TEXC_SWIZZLE_PSVITA },
    { "vita",      TEXC_SWIZZLE_PSVITA },
    { "x360",      TEXC_SWIZZLE_X360 },
    { "psp",       TEXC_SWIZZLE_PSP },
    { "3ds",       TEXC_SWIZZLE_3DS },
    { "n3ds",      TEXC_SWIZZLE_3DS },
    { "wiiu",      TEXC_SWIZZLE_WIIU },
    { "dx12_64kb", TEXC_SWIZZLE_DX12_64KB },
    { "dx12",      TEXC_SWIZZLE_DX12_64KB },
};

static texc_swizzle_mode parse_mode(const std::string &name) {
    std::string want = lower(name);
    for (const auto &m : k_modes)
        if (want == m.name) return m.mode;
    die("unknown swizzle mode '%s' (run `texc modes` for the list)",
        name.c_str());
    return TEXC_SWIZZLE_NONE;
}

static texc_pixel_profile parse_profile(const std::string &name) {
    std::string want = lower(name);
    for (int p = 1; p < TEXC_PROFILE_COUNT; p++) {
        const char *n = texc_profile_name((texc_pixel_profile)p);
        if (n && lower(n) == want) return (texc_pixel_profile)p;
    }
    die("unknown pixel profile '%s' (run `texc profiles` for the list)",
        name.c_str());
    return TEXC_PROFILE_INVALID;
}

/* --------------------------------------------------------------- options */

struct options {
    std::string command;
    std::string input;
    std::string output;
    uint64_t offset = 0;
    uint64_t size = 0;              /* 0 = to end of file / computed */
    texc_format format = TEXC_FORMAT_INVALID;
    uint32_t width = 0, height = 0;
    texc_swizzle_mode mode = TEXC_SWIZZLE_NONE;
    bool mode_set = false;
    uint32_t arg = 0;
    uint32_t alpha_threshold = 128;
    texc_pixel_profile src_profile = TEXC_PROFILE_INVALID;
    texc_pixel_profile dst_profile = TEXC_PROFILE_INVALID;
    uint32_t bpp = 4;
    uint32_t count = 0;
    uint32_t rect[4] = { 0, 0, 0, 0 };
    bool rect_set = false;
    char flip_dir = 'y';
    bool flip_y_after = false;      /* decode: flip result vertically */
    bool f32 = false;
    std::string palette;            /* paletted Wii formats: palette file  */
    texc_palette_format palette_format = TEXC_PALETTE_IA8;
    bool palette_format_set = false;
};

static texc_palette_format parse_palette_format(const std::string &name) {
    std::string want = lower(name);
    if (want == "ia8")    return TEXC_PALETTE_IA8;
    if (want == "rgb565") return TEXC_PALETTE_RGB565;
    if (want == "rgb5a3") return TEXC_PALETTE_RGB5A3;
    die("unknown palette format '%s' (ia8, rgb565 or rgb5a3)", name.c_str());
    return TEXC_PALETTE_IA8;
}

static void usage(const char *cmd) {
    std::string c = cmd ? lower(cmd) : "";
    if (c == "decode") printf(
"texc decode -i <file> -f <format> -w <W> -h <H> -o <out>\n"
"  Decode compressed texture data to an image.\n"
"    -i <file>            input file\n"
"    --offset <N>         byte offset of the texture data (default 0, 0x ok)\n"
"    --size <N>           bytes to read (default: rest of file)\n"
"    -f <format>          texture format (`texc formats`)\n"
"    -w/-h <N>            image dimensions in pixels (final size for the\n"
"                         *_A_ATLAS formats)\n"
"    --swizzle <mode>     UNSWIZZLE the data first (`texc modes`)\n"
"    --arg <N|auto>       mode-specific arg; 'auto' = 0xFFFFFFFF (Switch)\n"
"    --flip-y             flip the decoded image vertically\n"
"    --f32                decode to float RGBA (.raw output only)\n"
"    --palette <file>     palette for WII_C4/C8/C14X2 (big-endian 16-bit\n"
"                         entries, e.g. a G1TL entry); --offset applies to\n"
"                         the texture only\n"
"    --palette-format <f> ia8 | rgb565 | rgb5a3 (default ia8)\n"
"    -o <out>             .png / .tga = image, anything else = raw RGBA\n");
    else if (c == "encode") printf(
"texc encode -i <in.tga|in.raw> -f <format> [-w <W> -h <H>] -o <out>\n"
"  Encode an RGBA image to compressed texture data.\n"
"    -i <file>            .tga (uncompressed 24/32-bit; supplies W/H) or\n"
"                         raw RGBA8 (requires -w/-h)\n"
"    --offset/--size      window into the input file (raw input)\n"
"    -f <format>          target format (`texc formats`)\n"
"    --alpha-threshold <N> punchthrough cutoff (BC1/ETC2_RGBA1, default 128)\n"
"    --swizzle <mode>     RESWIZZLE the encoded data for that platform\n"
"    --arg <N|auto>       mode-specific arg\n"
"    --flip-y             flip the input image vertically before encoding\n"
"    -o <out>             output file (raw compressed / tiled bytes)\n");
    else if (c == "unswizzle" || c == "reswizzle") printf(
"texc %s -i <file> -f <format> -w <W> -h <H> -m <mode> -o <out>\n"
"  %s\n"
"    -i <file> [--offset/--size]   input data\n"
"    -f <format> -w/-h <N>         geometry the tiling is computed from\n"
"    -m <mode> [--arg <N|auto>]    platform layout (`texc modes`)\n"
"    -o <out>                      output file\n", c.c_str(),
        c == "unswizzle"
            ? "UNSWIZZLE: platform-tiled -> linear (use before decoding)."
            : "RESWIZZLE: linear -> platform-tiled (put the tiling back).");
    else if (c == "convert") printf(
"texc convert -i <in> --src-profile <P> --dst-profile <P> -o <out>\n"
"  Convert raw pixels between colour profiles (`texc profiles`).\n"
"    --count <N>          pixel count (default: derived from input size)\n"
"    --offset/--size      window into the input file\n");
    else if (c == "flip") printf(
"texc flip -i <in> -w <W> -h <H> [--bpp <N>] [--dir x|y] -o <out>\n"
"  Flip raw pixel data (default: vertical, --bpp 4).\n");
    else if (c == "crop") printf(
"texc crop -i <in> -w <W> -h <H> [--bpp <N>] --rect x,y,cw,ch -o <out>\n"
"  Copy a rectangle out of raw pixel data.\n");
    else if (c == "info") printf(
"texc info -f <format> -w <W> -h <H> [-m <mode> --arg <N|auto>]\n"
"  Print block geometry, encoded/decoded sizes and (with -m) the\n"
"  platform-tiled size.\n");
    else printf(
"texc — tex_codec command line (version %s)\n"
"\n"
"  texc decode      compressed texture -> .png / .tga / raw RGBA\n"
"  texc encode      .tga / raw RGBA -> compressed texture\n"
"  texc unswizzle   platform-tiled -> linear (use before decoding)\n"
"  texc reswizzle   linear -> platform-tiled (put the tiling back)\n"
"  texc convert     raw pixels between colour profiles\n"
"  texc flip        flip raw pixel data\n"
"  texc crop        crop raw pixel data\n"
"  texc info        sizes and block geometry for a format\n"
"  texc formats     list texture formats\n"
"  texc modes       list swizzle modes\n"
"  texc profiles    list pixel profiles\n"
"  texc version     version and build identification\n"
"  texc help <cmd>  detailed options for one command\n"
"\n"
"Common flags: -i input, -o output, --offset N (0x ok), --size N,\n"
"-f format, -w/-h pixels, -m/--swizzle mode, --arg N|auto.\n",
        texc_version_string());
}

static options parse_args(int argc, char **argv) {
    options o;
    o.command = lower(argv[1]);

    auto need = [&](int &i) -> const char * {
        if (i + 1 >= argc) die("missing value after %s", argv[i]);
        return argv[++i];
    };

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-i" || a == "--input")        o.input = need(i);
        else if (a == "-o" || a == "--output")  o.output = need(i);
        else if (a == "--offset")               o.offset = parse_u64(need(i), "offset");
        else if (a == "--size")                 o.size = parse_u64(need(i), "size");
        else if (a == "-f" || a == "--format")  o.format = parse_format(need(i));
        else if (a == "-w" || a == "--width")   o.width = (uint32_t)parse_u64(need(i), "width");
        else if (a == "-h" || a == "--height")  o.height = (uint32_t)parse_u64(need(i), "height");
        else if (a == "-m" || a == "--mode" || a == "--swizzle") {
            o.mode = parse_mode(need(i));
            o.mode_set = true;
        } else if (a == "--arg") {
            const char *v = need(i);
            o.arg = lower(v) == "auto" ? 0xFFFFFFFFu
                                       : (uint32_t)parse_u64(v, "arg");
        }
        else if (a == "--alpha-threshold")
            o.alpha_threshold = (uint32_t)parse_u64(need(i), "alpha-threshold");
        else if (a == "--src-profile") o.src_profile = parse_profile(need(i));
        else if (a == "--dst-profile") o.dst_profile = parse_profile(need(i));
        else if (a == "--bpp")   o.bpp = (uint32_t)parse_u64(need(i), "bpp");
        else if (a == "--count") o.count = (uint32_t)parse_u64(need(i), "count");
        else if (a == "--rect") {
            if (sscanf(need(i), "%u,%u,%u,%u", &o.rect[0], &o.rect[1],
                       &o.rect[2], &o.rect[3]) != 4)
                die("--rect wants x,y,w,h");
            o.rect_set = true;
        }
        else if (a == "--dir") {
            const char *v = need(i);
            if (lower(v) != "x" && lower(v) != "y") die("--dir wants x or y");
            o.flip_dir = (char)tolower((unsigned char)v[0]);
        }
        else if (a == "--flip-y") o.flip_y_after = true;
        else if (a == "--f32")    o.f32 = true;
        else if (a == "--palette") o.palette = need(i);
        else if (a == "--palette-format") {
            o.palette_format = parse_palette_format(need(i));
            o.palette_format_set = true;
        }
        else if (a == "--help")   { usage(o.command.c_str()); exit(0); }
        else die("unknown option '%s' (see `texc help %s`)", argv[i],
                 o.command.c_str());
    }
    return o;
}

/* -------------------------------------------------------------- commands */

static void require(const options &o, bool input, bool fmt, bool dims,
                    bool output) {
    if (input && o.input.empty())  die("-i <input> is required");
    if (fmt && o.format == TEXC_FORMAT_INVALID) die("-f <format> is required");
    if (dims && (!o.width || !o.height)) die("-w and -h are required");
    if (output && o.output.empty()) die("-o <output> is required");
}

static void cmd_decode(const options &o) {
    require(o, true, true, true, true);
    std::vector<uint8_t> src = read_file(o.input, o.offset, o.size);

    std::string ext = ext_of(o.output);
    if (o.f32) {
        if (ext == "png" || ext == "tga")
            die("--f32 output must be raw (.raw/.bin), not .%s", ext.c_str());
        if (o.mode_set && o.mode != TEXC_SWIZZLE_NONE)
            die("--f32 cannot be combined with --swizzle (unswizzle first)");
        std::vector<float> out((size_t)o.width * o.height * 4);
        int rc = texc_decode_f32(o.format, src.data(), src.size(), o.width,
                                 o.height, out.data(), out.size() * 4);
        if (rc != TEXC_OK) die_rc("decode_f32", rc);
        write_file(o.output, out.data(), out.size() * 4);
        return;
    }

    size_t out_size = texc_decoded_size(o.width, o.height);
    std::vector<uint8_t> rgba(out_size);
    int rc;
    if (texc_is_paletted(o.format)) {
        if (o.palette.empty())
            die("%s is paletted: pass --palette <file> "
                "[--palette-format ia8|rgb565|rgb5a3]",
                texc_format_name(o.format));
        if (o.mode_set && o.mode != TEXC_SWIZZLE_NONE)
            die("--swizzle cannot be combined with a paletted format "
                "(unswizzle first)");
        std::vector<uint8_t> pal = read_file(o.palette, 0, 0);
        size_t need = texc_palette_size(o.format);
        if (pal.size() < need)
            die("palette '%s' is %zu bytes, %s needs %zu", o.palette.c_str(),
                pal.size(), texc_format_name(o.format), need);
        rc = texc_decode_paletted(o.format, src.data(), src.size(),
                                  o.width, o.height, pal.data(), pal.size(),
                                  o.palette_format, rgba.data(), out_size);
    } else {
        rc = (o.mode_set && o.mode != TEXC_SWIZZLE_NONE)
            ? texc_decode_swizzled(o.mode, o.format, o.width, o.height,
                                   src.data(), src.size(), rgba.data(),
                                   out_size, o.arg)
            : texc_decode(o.format, src.data(), src.size(), o.width,
                          o.height, rgba.data(), out_size);
    }
    if (rc != TEXC_OK) die_rc("decode", rc);

    if (o.flip_y_after)
        texc_flip_y(rgba.data(), rgba.size(), rgba.data(), rgba.size(),
                    o.width, o.height, 4);

    if (ext == "png")      write_png(o.output, rgba.data(), o.width, o.height);
    else if (ext == "tga") write_tga(o.output, rgba.data(), o.width, o.height);
    else                   write_file(o.output, rgba.data(), rgba.size());
}

static void cmd_encode(const options &o) {
    require(o, true, true, false, true);
    std::vector<uint8_t> in = read_file(o.input, o.offset, o.size);

    uint32_t w = o.width, h = o.height;
    std::vector<uint8_t> rgba;
    if (ext_of(o.input) == "tga") {
        rgba = read_tga(in, &w, &h);
        if ((o.width && o.width != w) || (o.height && o.height != h))
            die("-w/-h (%ux%u) disagree with the TGA (%ux%u)",
                o.width, o.height, w, h);
    } else {
        if (!w || !h) die("-w and -h are required for raw RGBA input");
        if (in.size() < texc_decoded_size(w, h))
            die("input too small for %ux%u RGBA (%zu < %zu bytes)", w, h,
                in.size(), texc_decoded_size(w, h));
        rgba = std::move(in);
    }

    if (o.flip_y_after)
        texc_flip_y(rgba.data(), rgba.size(), rgba.data(), rgba.size(),
                    w, h, 4);

    size_t enc_size = texc_encoded_size(o.format, w, h);
    if (!enc_size) die("cannot size %s at %ux%u",
                       texc_format_name(o.format), w, h);
    std::vector<uint8_t> enc(enc_size);
    texc_encode_options opts;
    texc_encode_options_init(&opts);
    opts.alpha_threshold = o.alpha_threshold;
    int rc = texc_encode_ex(o.format, rgba.data(), rgba.size(), w, h,
                            enc.data(), enc.size(), &opts);
    if (rc != TEXC_OK) die_rc("encode", rc);

    if (o.mode_set && o.mode != TEXC_SWIZZLE_NONE) {
        size_t tiled_size = texc_swizzled_size(o.mode, o.format, w, h, o.arg);
        if (!tiled_size) die("cannot size the tiled layout");
        std::vector<uint8_t> tiled(tiled_size);
        rc = texc_reswizzle(o.mode, o.format, w, h, enc.data(), enc.size(),
                            tiled.data(), tiled.size(), o.arg);
        if (rc != TEXC_OK) die_rc("reswizzle", rc);
        write_file(o.output, tiled.data(), tiled.size());
    } else {
        write_file(o.output, enc.data(), enc.size());
    }
}

static void cmd_swizzle(const options &o, bool to_linear) {
    require(o, true, true, true, true);
    if (!o.mode_set) die("-m <mode> is required");
    std::vector<uint8_t> src = read_file(o.input, o.offset, o.size);

    size_t out_size = to_linear
        ? texc_encoded_size(o.format, o.width, o.height)
        : texc_swizzled_size(o.mode, o.format, o.width, o.height, o.arg);
    if (!out_size) die("cannot size the output layout");
    std::vector<uint8_t> out(out_size);
    int rc = to_linear
        ? texc_unswizzle(o.mode, o.format, o.width, o.height, src.data(),
                         src.size(), out.data(), out.size(), o.arg)
        : texc_reswizzle(o.mode, o.format, o.width, o.height, src.data(),
                         src.size(), out.data(), out.size(), o.arg);
    if (rc != TEXC_OK) die_rc(to_linear ? "unswizzle" : "reswizzle", rc);
    write_file(o.output, out.data(), out.size());
}

static void cmd_convert(const options &o) {
    require(o, true, false, false, true);
    if (o.src_profile == TEXC_PROFILE_INVALID ||
        o.dst_profile == TEXC_PROFILE_INVALID)
        die("--src-profile and --dst-profile are required");
    std::vector<uint8_t> src = read_file(o.input, o.offset, o.size);

    uint32_t sbpp = texc_profile_bytes_per_pixel(o.src_profile);
    uint32_t dbpp = texc_profile_bytes_per_pixel(o.dst_profile);
    uint32_t count = o.count ? o.count : (uint32_t)(src.size() / sbpp);
    std::vector<uint8_t> out((size_t)count * dbpp);
    int rc = texc_convert_profile(o.src_profile, src.data(), src.size(),
                                  o.dst_profile, out.data(), out.size(),
                                  count);
    if (rc != TEXC_OK) die_rc("convert_profile", rc);
    write_file(o.output, out.data(), out.size());
}

static void cmd_flip(const options &o) {
    require(o, true, false, true, true);
    std::vector<uint8_t> src = read_file(o.input, o.offset, o.size);
    std::vector<uint8_t> out(src.size());
    int rc = (o.flip_dir == 'x')
        ? texc_flip_x(src.data(), src.size(), out.data(), out.size(),
                      o.width, o.height, o.bpp)
        : texc_flip_y(src.data(), src.size(), out.data(), out.size(),
                      o.width, o.height, o.bpp);
    if (rc != TEXC_OK) die_rc("flip", rc);
    write_file(o.output, out.data(),
               (size_t)o.width * o.height * o.bpp);
}

static void cmd_crop(const options &o) {
    require(o, true, false, true, true);
    if (!o.rect_set) die("--rect x,y,w,h is required");
    std::vector<uint8_t> src = read_file(o.input, o.offset, o.size);
    std::vector<uint8_t> out((size_t)o.rect[2] * o.rect[3] * o.bpp);
    int rc = texc_crop(src.data(), src.size(), o.width, o.height, o.bpp,
                       o.rect[0], o.rect[1], o.rect[2], o.rect[3],
                       out.data(), out.size());
    if (rc != TEXC_OK) die_rc("crop", rc);
    write_file(o.output, out.data(), out.size());
}

static void cmd_info(const options &o) {
    require(o, false, true, true, false);
    uint32_t bw, bh, bb;
    texc_block_dims(o.format, &bw, &bh, &bb);
    printf("format          %s\n", texc_format_name(o.format));
    printf("block           %ux%u, %u bytes\n", bw, bh, bb);
    printf("image           %ux%u\n", o.width, o.height);
    printf("encoded size    %zu bytes\n",
           texc_encoded_size(o.format, o.width, o.height));
    printf("decoded size    %zu bytes (RGBA8)\n",
           texc_decoded_size(o.width, o.height));
    if (o.mode_set) {
        size_t s = texc_swizzled_size(o.mode, o.format, o.width, o.height,
                                      o.arg);
        printf("tiled size      %zu bytes\n", s);
    }
}

static void cmd_version(bool verbose) {
    if (!verbose) {
        printf("%s\n", texc_version_string());
        return;
    }
    printf("%s\n", texc_build_info());
    /* A dynamically linked texc can outlive the header it was built
     * against; surfacing both makes a mismatch obvious. */
    if (texc_version() != TEXC_VERSION_NUMBER)
        printf("warning: CLI built against header %s but linked library "
               "reports %s\n", TEXC_VERSION_STRING, texc_version_string());
}

static void cmd_list(const std::string &what) {
    if (what == "formats") {
        for (int f = 1; f < TEXC_FORMAT_COUNT; f++) {
            uint32_t bw, bh, bb;
            texc_block_dims((texc_format)f, &bw, &bh, &bb);
            printf("  %-26s %2ux%-2u block, %2u bytes\n",
                   texc_format_name((texc_format)f), bw, bh, bb);
        }
    } else if (what == "modes") {
        printf("  none ps4 ps5 switch psvita x360 psp 3ds wiiu dx12_64kb\n"
               "  (--arg: switch = block height log2 or 'auto';\n"
               "   wiiu = GX2 swizzle value; x360 = pitch override)\n");
    } else {
        for (int p = 1; p < TEXC_PROFILE_COUNT; p++)
            printf("  %-12s %u bytes/px\n",
                   texc_profile_name((texc_pixel_profile)p),
                   texc_profile_bytes_per_pixel((texc_pixel_profile)p));
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(nullptr); return 1; }
    std::string cmd = lower(argv[1]);

    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage(argc > 2 ? argv[2] : nullptr);
        return 0;
    }
    if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        /* `texc version` prints full build info; `--short` just the number
         * (for scripts: VER=$(texc version --short) ). */
        bool short_form = (argc > 2 && lower(argv[2]) == "--short");
        cmd_version(!short_form);
        return 0;
    }
    if (cmd == "formats" || cmd == "modes" || cmd == "profiles") {
        cmd_list(cmd);
        return 0;
    }

    options o = parse_args(argc, argv);
    if (cmd == "decode")         cmd_decode(o);
    else if (cmd == "encode")    cmd_encode(o);
    else if (cmd == "unswizzle") cmd_swizzle(o, true);
    else if (cmd == "reswizzle" || cmd == "swizzle") cmd_swizzle(o, false);
    else if (cmd == "convert")   cmd_convert(o);
    else if (cmd == "flip")      cmd_flip(o);
    else if (cmd == "crop")      cmd_crop(o);
    else if (cmd == "info")      cmd_info(o);
    else { usage(nullptr); die("unknown command '%s'", argv[1]); }
    return 0;
}
