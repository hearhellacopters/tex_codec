/*
 * tex_codec.h - portable GPU texture codec library, flat C API.
 *
 * Decodes and encodes BC1-BC7, ETC1/ETC2/EAC, PVRTC1/PVRTC2, ASTC and ATC
 * texture data, and provides platform unswizzling (G1T / console layouts).
 *
 * Conventions:
 *   - Decoded images are 8-bit RGBA (R = byte 0), row-major, top-left origin,
 *     tightly packed (stride = width * 4).
 *   - Compressed data is a row-major sequence of blocks, no padding between
 *     block rows, exactly texc_encoded_size() bytes.
 *   - Images whose dimensions are not multiples of the block size use edge
 *     clamping: encoders replicate border pixels, decoders clip the block.
 *   - All functions return TEXC_OK (0) on success or a negative texc_result.
 */
#ifndef TEX_CODEC_H
#define TEX_CODEC_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(TEXC_SHARED)
#  ifdef TEXC_BUILD
#    define TEXC_API __declspec(dllexport)
#  else
#    define TEXC_API __declspec(dllimport)
#  endif
#elif defined(__EMSCRIPTEN__)
#  include <emscripten/emscripten.h>
#  define TEXC_API EMSCRIPTEN_KEEPALIVE
#elif defined(__GNUC__) && defined(TEXC_SHARED)
#  define TEXC_API __attribute__((visibility("default")))
#else
#  define TEXC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- version
 * THIS IS THE SINGLE SOURCE OF TRUTH for the library version. CMake parses
 * these three macros to set the project version, the installed package
 * version and the Windows file-version resource stamped into
 * tex_codec.dll / texc.exe - so bumping the version means editing only
 * these lines.
 *
 * Semantics: MAJOR = breaking ABI/API change, MINOR = backward-compatible
 * additions (new formats/functions are only ever APPENDED to the enums so
 * existing values stay stable), PATCH = fixes only. */
#define TEXC_VERSION_MAJOR 1
#define TEXC_VERSION_MINOR 4
#define TEXC_VERSION_PATCH 0

#define TEXC_VERSION_STRINGIZE_(x) #x
#define TEXC_VERSION_STRINGIZE(x) TEXC_VERSION_STRINGIZE_(x)

/* e.g. "1.2.0" - the version this HEADER declares, at compile time. */
#define TEXC_VERSION_STRING                      \
    TEXC_VERSION_STRINGIZE(TEXC_VERSION_MAJOR) "." \
    TEXC_VERSION_STRINGIZE(TEXC_VERSION_MINOR) "." \
    TEXC_VERSION_STRINGIZE(TEXC_VERSION_PATCH)

/* Packed as (major << 16) | (minor << 8) | patch, for numeric comparisons:
 *   #if TEXC_VERSION_NUMBER >= TEXC_VERSION_ENCODE(1, 1, 0) */
#define TEXC_VERSION_ENCODE(maj, min, pat) \
    (((maj) << 16) | ((min) << 8) | (pat))
#define TEXC_VERSION_NUMBER                                  \
    TEXC_VERSION_ENCODE(TEXC_VERSION_MAJOR, TEXC_VERSION_MINOR, \
                        TEXC_VERSION_PATCH)

/* ---------------------------------------------------------------- formats */

typedef enum texc_format {
    TEXC_FORMAT_INVALID = 0,

    /* Raw (passthrough / convenience) */
    TEXC_FORMAT_RGBA8,            /* 32bpp raw, identity codec              */

    /* BC / DXT family */
    TEXC_FORMAT_BC1,              /* DXT1, 1-bit alpha, 8B/block            */
    TEXC_FORMAT_BC2,              /* DXT3, explicit alpha, 16B/block        */
    TEXC_FORMAT_BC3,              /* DXT5, interpolated alpha, 16B/block    */
    TEXC_FORMAT_BC4,              /* ATI1 / RGTC1 unsigned, 8B/block        */
    TEXC_FORMAT_BC4_SNORM,
    TEXC_FORMAT_BC5,              /* ATI2 / 3Dc / RGTC2 unsigned, 16B/block */
    TEXC_FORMAT_BC5_SNORM,
    TEXC_FORMAT_BC6H_UF16,        /* HDR, unsigned half floats, 16B/block   */
    TEXC_FORMAT_BC6H_SF16,        /* HDR, signed half floats, 16B/block     */
    TEXC_FORMAT_BC7,              /* 16B/block                              */

    /* ETC / EAC */
    TEXC_FORMAT_ETC1_RGB,
    TEXC_FORMAT_ETC2_RGB,
    TEXC_FORMAT_ETC2_RGBA1,       /* RGB + punchthrough 1-bit alpha         */
    TEXC_FORMAT_ETC2_RGBA8,       /* ETC2 colour + EAC alpha, 16B/block     */
    TEXC_FORMAT_EAC_R11,
    TEXC_FORMAT_EAC_R11_SIGNED,
    TEXC_FORMAT_EAC_RG11,
    TEXC_FORMAT_EAC_RG11_SIGNED,

    /* PVRTC (dimensions must be >= one block; PVRTC1 requires power-of-two) */
    TEXC_FORMAT_PVRTC1_2BPP_RGB,
    TEXC_FORMAT_PVRTC1_2BPP_RGBA,
    TEXC_FORMAT_PVRTC1_4BPP_RGB,
    TEXC_FORMAT_PVRTC1_4BPP_RGBA,
    TEXC_FORMAT_PVRTC2_2BPP,
    TEXC_FORMAT_PVRTC2_4BPP,

    /* ASTC (LDR + HDR blocks decoded transparently; 16B/block) */
    TEXC_FORMAT_ASTC_4x4,
    TEXC_FORMAT_ASTC_5x4,
    TEXC_FORMAT_ASTC_5x5,
    TEXC_FORMAT_ASTC_6x5,
    TEXC_FORMAT_ASTC_6x6,
    TEXC_FORMAT_ASTC_8x5,
    TEXC_FORMAT_ASTC_8x6,
    TEXC_FORMAT_ASTC_8x8,
    TEXC_FORMAT_ASTC_10x5,
    TEXC_FORMAT_ASTC_10x6,
    TEXC_FORMAT_ASTC_10x8,
    TEXC_FORMAT_ASTC_10x10,
    TEXC_FORMAT_ASTC_12x10,
    TEXC_FORMAT_ASTC_12x12,

    /* ATC (Adreno) */
    TEXC_FORMAT_ATC_RGB,          /* ATC_RGB_AMD, 8B/block                  */
    TEXC_FORMAT_ATC_RGBA_EXPLICIT,     /* ATC_RGBA_EXPLICIT_ALPHA_AMD, 16B  */
    TEXC_FORMAT_ATC_RGBA_INTERPOLATED, /* ATC_RGBA_INTERPOLATED_ALPHA_AMD   */

    /* G1T alpha-atlas dual-plane formats (KTGL trick for codecs without
     * native alpha): the compressed data is ONE image of the base codec at
     * DOUBLE height - RGB content in the top half, the alpha channel as a
     * grayscale map in the bottom half (8bpp effective for 4bpp codecs).
     * texc_decode folds the atlas back: A = R of the bottom-half texel
     * (exactly what KTGL does with bHasAlphaAtlas). texc_encode splits RGBA
     * into the two planes. width/height passed to the API are the FINAL
     * image dimensions (the half-height RGB image); sizes and swizzling are
     * computed on the double-height layout automatically.
     * Note: for KTGL_PIXEL_FORMAT_ETC2RGBA8 (0x71) the engine itself skips
     * the fold (bSkipAlphaAtlas, likely a KTGL bug) - this library folds;
     * decode as TEXC_FORMAT_ETC2_RGB at 2*height yourself if you need the
     * engine-accurate ignored-alpha output. */
    TEXC_FORMAT_ETC1_RGB_A_ATLAS,        /* KTGL ETC1RGBETC1A   (0x6F)      */
    TEXC_FORMAT_PVRTC1_4BPP_RGB_A_ATLAS, /* KTGL PVRT4RGBPVRT4A (0x70)      */
    TEXC_FORMAT_ETC2_RGB_A_ATLAS,        /* KTGL ETC2RGBA8      (0x71)      */

    /* PICA200 (Nintendo 3DS) ETC1. Same ETC1 codec, different container:
     * the image is stored as 8x8 pixel TILES in row-major order, each tile
     * holding four 4x4 ETC1 blocks in the order (0,0), (4,0), (0,4), (4,4),
     * and every 64-bit ETC1 block is byte-reversed relative to the standard
     * big-endian layout. RGB8A4 additionally puts 8 bytes of 4-bit alpha in
     * FRONT of each colour block (nibble index x*4 + y, ETC1's own pixel
     * order, expanded as (a << 4) | a).
     *
     * Block geometry is therefore reported as 8x8 with 32 bytes (RGB8,
     * 4bpp) or 64 bytes (RGB8A4, 8bpp) per tile, so sizes round up to whole
     * tiles exactly as the hardware stores them.
     *
     * Layout verified against devkitPro tex3ds (which encodes via rg-etc1)
     * and gdkchan/SPICA. Note SPICA also flips its output vertically as its
     * own convention; that is not part of the format, so this library does
     * not (use texc_flip_y if you want that orientation). */
    TEXC_FORMAT_PICA_ETC1_RGB8,   /* 3DS GPU_ETC1,   4bpp, 32B/8x8 tile    */
    TEXC_FORMAT_PICA_ETC1_RGB8A4, /* 3DS GPU_ETC1A4, 8bpp, 64B/8x8 tile    */

    TEXC_FORMAT_COUNT
} texc_format;

/* ----------------------------------------------------------- result codes */

typedef enum texc_result {
    TEXC_OK                   =  0,
    TEXC_ERR_INVALID_ARG      = -1,
    TEXC_ERR_BUFFER_TOO_SMALL = -2,
    TEXC_ERR_UNSUPPORTED      = -3,   /* format/operation not implemented   */
    TEXC_ERR_BAD_DATA         = -4,   /* malformed compressed input         */
    TEXC_ERR_OUT_OF_MEMORY    = -5,
    TEXC_ERR_BAD_DIMENSIONS   = -6    /* e.g. PVRTC1 non-power-of-two       */
} texc_result;

/* ------------------------------------------------------------------ query */

/* Version of the LIBRARY BINARY as (major << 16) | (minor << 8) | patch.
 * Compare against TEXC_VERSION_NUMBER (the header you compiled against) to
 * detect a header/binary mismatch when linking dynamically. */
TEXC_API uint32_t texc_version(void);

/* Version of the library binary as "1.1.0". Never NULL. */
TEXC_API const char *texc_version_string(void);

/* One-line build identification for logs and bug reports, e.g.
 * "tex_codec 1.1.0 (git 3f2a1b8, built Aug 19 2026, MSVC 19.44, x64)".
 * The git hash is stamped in at CMake configure time when the source is a
 * git checkout ("unknown" otherwise). Never NULL. */
TEXC_API const char *texc_build_info(void);

/* Human-readable name of a format ("BC7", "ASTC_6x6", ...); NULL if invalid */
TEXC_API const char *texc_format_name(texc_format format);

/* Short description of a result code. */
TEXC_API const char *texc_result_str(int result);

/* Block geometry. Any output pointer may be NULL. Returns TEXC_OK or error.
 * For RGBA8, block is 1x1 with 4 bytes. */
TEXC_API int texc_block_dims(texc_format format,
                             uint32_t *block_width, uint32_t *block_height,
                             uint32_t *block_bytes);

/* Compressed size in bytes of a width x height image (0 if invalid). */
TEXC_API size_t texc_encoded_size(texc_format format,
                                  uint32_t width, uint32_t height);

/* Decoded RGBA8 size in bytes: width * height * 4 (0 on overflow). */
TEXC_API size_t texc_decoded_size(uint32_t width, uint32_t height);

/* 1 if decode / encode is implemented for the format, else 0. */
TEXC_API int texc_can_decode(texc_format format);
TEXC_API int texc_can_encode(texc_format format);

/* --------------------------------------------------------- decode / encode */

/* Decode compressed texture data to 8-bit RGBA.
 * src_size must be >= texc_encoded_size(format, w, h);
 * dst_size must be >= texc_decoded_size(w, h).
 * HDR sources (BC6H, ASTC HDR blocks) are tone-clamped to [0,1]. */
TEXC_API int texc_decode(texc_format format,
                         const uint8_t *src, size_t src_size,
                         uint32_t width, uint32_t height,
                         uint8_t *dst, size_t dst_size);

/* Decode to 32-bit float RGBA (full range for HDR formats).
 * dst_size must be >= width * height * 16. */
TEXC_API int texc_decode_f32(texc_format format,
                             const uint8_t *src, size_t src_size,
                             uint32_t width, uint32_t height,
                             float *dst, size_t dst_size);

/* Encode 8-bit RGBA to compressed texture data.
 * src_size must be >= texc_decoded_size(w, h);
 * dst_size must be >= texc_encoded_size(format, w, h). */
TEXC_API int texc_encode(texc_format format,
                         const uint8_t *src, size_t src_size,
                         uint32_t width, uint32_t height,
                         uint8_t *dst, size_t dst_size);

/* Optional encoder tuning. Zero-init (or texc_encode_options_init) gives
 * defaults; the struct may grow - struct_size versions it. */
typedef struct texc_encode_options {
    uint32_t struct_size;     /* set to sizeof(texc_encode_options)          */
    uint32_t alpha_threshold; /* pixels with alpha < threshold become
                               * transparent in punchthrough formats
                               * (BC1 3-colour mode, ETC2_RGBA1). Default 128 */
    uint32_t flags;           /* reserved, must be 0                         */
    uint32_t reserved[5];     /* reserved, must be 0                         */
} texc_encode_options;

/* Fill an options struct with the library defaults. */
TEXC_API void texc_encode_options_init(texc_encode_options *opts);

/* texc_encode with options; opts may be NULL (same as texc_encode). */
TEXC_API int texc_encode_ex(texc_format format,
                            const uint8_t *src, size_t src_size,
                            uint32_t width, uint32_t height,
                            uint8_t *dst, size_t dst_size,
                            const texc_encode_options *opts);

/* Encode 32-bit float RGBA (meaningful for BC6H; others are clamped). */
TEXC_API int texc_encode_f32(texc_format format,
                             const float *src, size_t src_size,
                             uint32_t width, uint32_t height,
                             uint8_t *dst, size_t dst_size);

/* ------------------------------------------------------------- unswizzle */

/* Hardware layouts supported by texc_swizzle / texc_unswizzle.
 * These operate on the *compressed* (or raw) data, rearranging blocks /
 * texels from a platform-tiled layout to linear row-major and back. */
typedef enum texc_swizzle_mode {
    TEXC_SWIZZLE_NONE = 0,        /* memcpy                                  */
    TEXC_SWIZZLE_PS4,             /* Orbis/GNF 8x8-micro-tile Morton         */
    TEXC_SWIZZLE_PS5,             /* Prospero variant                        */
    TEXC_SWIZZLE_SWITCH,          /* Tegra X1 block-linear GOB (see `arg`)   */
    TEXC_SWIZZLE_PSVITA,          /* GXM Morton / Z-order                    */
    TEXC_SWIZZLE_X360,            /* Xenos macro tiling                      */
    TEXC_SWIZZLE_PSP,             /* 16-byte x 8-row tiles                   */
    TEXC_SWIZZLE_3DS,             /* 8x8 Z-order tiles                       */
    TEXC_SWIZZLE_WIIU,            /* GX2 tiled (addrlib subset, see `arg`)   */
    TEXC_SWIZZLE_DX12_64KB,       /* D3D12 64KB standard swizzle             */
    TEXC_SWIZZLE_MODE_COUNT
} texc_swizzle_mode;

/* Size in bytes of the swizzled representation (may exceed the linear size
 * because of tile padding). 0 if invalid.
 * `arg` is mode-specific (see texc_unswizzle). */
TEXC_API size_t texc_swizzled_size(texc_swizzle_mode mode, texc_format format,
                                   uint32_t width, uint32_t height,
                                   uint32_t arg);

/* Size in bytes of the LINEAR side of a conversion - the output of
 * texc_unswizzle, the input of texc_reswizzle. This is normally just
 * texc_encoded_size(format, width, height), but PS Vita raw honours the
 * bytes-per-pixel `arg` override, so it cannot be derived from the format
 * alone; prefer this whenever you pass a non-zero `arg`. 0 if invalid. */
TEXC_API size_t texc_unswizzled_size(texc_swizzle_mode mode,
                                     texc_format format,
                                     uint32_t width, uint32_t height,
                                     uint32_t arg);

/* Convert platform-tiled data to linear row-major block order.
 * width/height are in PIXELS of the original image; block geometry is taken
 * from `format`. src holds texc_swizzled_size() bytes, dst receives
 * texc_encoded_size() bytes.
 *
 * `arg` per mode:
 *   SWITCH     : log2 of block height in GOBs (0..5); pass 0xFFFFFFFF to
 *                auto-select from the mip height like the hardware does.
 *   WIIU       : GX2 swizzle value from the texture header (usually 0).
 *   X360       : texel pitch override in bytes, or 0 for default.
 *   PSVITA     : for RAW (non-block) formats, bytes per pixel, or 0 to use
 *                the format's own size. G1T drives the Vita raw path from
 *                bitsPerPixel and ships 8/16/24/32bpp raw textures, so pass
 *                3 to deswizzle a 24bpp image as RGBA8-shaped data. Ignored
 *                for block-compressed formats.
 *   all others : pass 0.
 *
 * Note on PS Vita RAW: this reproduces DeswizzlePSVitaRaw exactly, which
 * uses the raw width (the tiled buffer is exactly w*h*bpp, NOT rounded up
 * to whole 32x32 tiles) and silently drops texels whose mapped offset falls
 * outside the image. That happens only when a dimension is not a multiple
 * of 32, and it makes the mapping non-invertible there.
 */
/* UNSWIZZLE: platform-tiled -> linear row-major (the direction you need
 * BEFORE decoding console data). */
TEXC_API int texc_unswizzle(texc_swizzle_mode mode, texc_format format,
                            uint32_t width, uint32_t height,
                            const uint8_t *src, size_t src_size,
                            uint8_t *dst, size_t dst_size,
                            uint32_t arg);

/* RESWIZZLE: linear row-major -> platform-tiled (the exact inverse of
 * texc_unswizzle; the direction you need to put the platform tiling BACK
 * when repacking data for the target hardware, e.g. into a G1T).
 * texc_swizzle and texc_reswizzle are the same function under two names -
 * texc_reswizzle exists so call sites read unambiguously. */
TEXC_API int texc_swizzle(texc_swizzle_mode mode, texc_format format,
                          uint32_t width, uint32_t height,
                          const uint8_t *src, size_t src_size,
                          uint8_t *dst, size_t dst_size,
                          uint32_t arg);
TEXC_API int texc_reswizzle(texc_swizzle_mode mode, texc_format format,
                            uint32_t width, uint32_t height,
                            const uint8_t *src, size_t src_size,
                            uint8_t *dst, size_t dst_size,
                            uint32_t arg);

/* Convenience: unswizzle + decode in one call (src is platform-tiled
 * compressed data, dst is RGBA8). */
TEXC_API int texc_decode_swizzled(texc_swizzle_mode mode, texc_format format,
                                  uint32_t width, uint32_t height,
                                  const uint8_t *src, size_t src_size,
                                  uint8_t *dst, size_t dst_size,
                                  uint32_t arg);

/* ------------------------------------------------------- image utilities */
/* Colour profile conversion and flip/crop, modelled on tex-decoder's
 * profiler.ts (convertProfile) and flipper.ts (flipImage / cropImage).   */

/* Raw pixel layouts. The name encodes channel order and bit width; the
 * suffix encodes the value type: no suffix = unsigned normalised,
 * I = signed (two's complement), 16F = half float, 32F = float.
 * Values are stable - new profiles are only appended.
 *
 * Bit packing: byte-aligned channels (multiples of 8 bits) are stored in
 * byte order with multi-byte values little-endian (RGBA8: R = byte 0;
 * R16F: LE half). Sub-byte packed profiles (RGB565, RGBA4, RGBA51,
 * RGB10_A2) pack channels MSB-first in byte order, matching tex-decoder's
 * sequential bit reader. */
typedef enum texc_pixel_profile {
    TEXC_PROFILE_INVALID = 0,
    /* 8-bit unsigned, single channel */
    TEXC_PROFILE_A8, TEXC_PROFILE_R8, TEXC_PROFILE_G8, TEXC_PROFILE_B8,
    /* 8-bit unsigned, two channels */
    TEXC_PROFILE_RG8, TEXC_PROFILE_RB8, TEXC_PROFILE_GR8,
    TEXC_PROFILE_GB8, TEXC_PROFILE_BR8, TEXC_PROFILE_BG8,
    /* 8-bit unsigned, three channels (all permutations) */
    TEXC_PROFILE_RGB8, TEXC_PROFILE_RBG8, TEXC_PROFILE_GRB8,
    TEXC_PROFILE_GBR8, TEXC_PROFILE_BRG8, TEXC_PROFILE_BGR8,
    /* 8-bit unsigned, alpha-first four channels */
    TEXC_PROFILE_ARGB8, TEXC_PROFILE_ARBG8, TEXC_PROFILE_AGRB8,
    TEXC_PROFILE_AGBR8, TEXC_PROFILE_ABRG8, TEXC_PROFILE_ABGR8,
    /* 8-bit unsigned, alpha-last four channels */
    TEXC_PROFILE_RGBA8, TEXC_PROFILE_RBGA8, TEXC_PROFILE_GRBA8,
    TEXC_PROFILE_GBRA8, TEXC_PROFILE_BRGA8, TEXC_PROFILE_BGRA8,
    /* packed */
    TEXC_PROFILE_RGB565, TEXC_PROFILE_BGR565, TEXC_PROFILE_RGBA4,
    TEXC_PROFILE_RGBA51, TEXC_PROFILE_RGB10_A2, TEXC_PROFILE_RGB10_A2I,
    /* 8-bit signed */
    TEXC_PROFILE_A8I, TEXC_PROFILE_R8I, TEXC_PROFILE_RG8I,
    TEXC_PROFILE_RGB8I, TEXC_PROFILE_RGBA8I, TEXC_PROFILE_ARGB8I,
    TEXC_PROFILE_BGR8I, TEXC_PROFILE_BGRA8I, TEXC_PROFILE_ABGR8I,
    /* 16-bit half float */
    TEXC_PROFILE_A16F, TEXC_PROFILE_R16F, TEXC_PROFILE_RG16F,
    TEXC_PROFILE_RGB16F, TEXC_PROFILE_RGBA16F, TEXC_PROFILE_ARGB16F,
    /* 16-bit unsigned */
    TEXC_PROFILE_R16, TEXC_PROFILE_RG16, TEXC_PROFILE_RGB16,
    TEXC_PROFILE_RGBA16,
    /* 16-bit signed */
    TEXC_PROFILE_A16I, TEXC_PROFILE_R16I, TEXC_PROFILE_RG16I,
    TEXC_PROFILE_RGB16I, TEXC_PROFILE_RGBA16I,
    /* 32-bit float */
    TEXC_PROFILE_A32F, TEXC_PROFILE_R32F, TEXC_PROFILE_RG32F,
    TEXC_PROFILE_RGB32F, TEXC_PROFILE_RGBA32F,
    /* 32-bit unsigned */
    TEXC_PROFILE_A32, TEXC_PROFILE_R32, TEXC_PROFILE_RG32,
    TEXC_PROFILE_RGB32, TEXC_PROFILE_RGBA32,
    /* 32-bit signed */
    TEXC_PROFILE_R32I, TEXC_PROFILE_RG32I, TEXC_PROFILE_RGB32I,
    TEXC_PROFILE_RGBA32I,
    TEXC_PROFILE_COUNT
} texc_pixel_profile;

/* Profile name ("RGBA8", "RGB565", ...); NULL if invalid. */
TEXC_API const char *texc_profile_name(texc_pixel_profile profile);

/* Bytes per pixel of a profile (0 if invalid). */
TEXC_API uint32_t texc_profile_bytes_per_pixel(texc_pixel_profile profile);

/* Convert raw pixels between colour profiles (tex-decoder convertProfile
 * semantics): values scale proportionally between each type's range,
 * a missing source alpha becomes fully opaque, missing colour channels
 * become 0, float profiles treat 1.0 as the integer maximum.
 * pixel_count 0 = derive from src_size. src and dst must not overlap. */
TEXC_API int texc_convert_profile(texc_pixel_profile src_profile,
                                  const uint8_t *src, size_t src_size,
                                  texc_pixel_profile dst_profile,
                                  uint8_t *dst, size_t dst_size,
                                  uint32_t pixel_count);

/* Flip raw pixel rows vertically (tex-decoder flipImage). Any
 * bytes_per_pixel >= 1; dst may equal src for an in-place flip. */
TEXC_API int texc_flip_y(const uint8_t *src, size_t src_size,
                         uint8_t *dst, size_t dst_size,
                         uint32_t width, uint32_t height,
                         uint32_t bytes_per_pixel);

/* Mirror raw pixels horizontally; dst may equal src. */
TEXC_API int texc_flip_x(const uint8_t *src, size_t src_size,
                         uint8_t *dst, size_t dst_size,
                         uint32_t width, uint32_t height,
                         uint32_t bytes_per_pixel);

/* Copy the rectangle (x, y, crop_width, crop_height) out of a raw image
 * (tex-decoder cropImage). The rectangle must lie inside the source;
 * src and dst must not overlap. dst receives
 * crop_width * crop_height * bytes_per_pixel bytes. */
TEXC_API int texc_crop(const uint8_t *src, size_t src_size,
                       uint32_t width, uint32_t height,
                       uint32_t bytes_per_pixel,
                       uint32_t x, uint32_t y,
                       uint32_t crop_width, uint32_t crop_height,
                       uint8_t *dst, size_t dst_size);

/* --------------------------------------------------- memory (FFI / WASM) */

TEXC_API void *texc_alloc(size_t size);
TEXC_API void  texc_free(void *ptr);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TEX_CODEC_H */
