# tex_codec

A standalone, portable C++ library (with a flat C API and WebAssembly build)
for **decoding and encoding** GPU compressed texture formats, plus platform
**unswizzling** and **reswizzling** for console texture layouts.

No external dependencies. One header, one static/shared library.

Includes Web Assembly for `.wasm`, `.js` and `.ts` output.

## Supported formats

| Family | Formats | Decode | Encode |
|---|---|---|---|
| BC / DXT | BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC4 ± SNORM (ATI1/RGTC1), BC5 ± SNORM (ATI2/3Dc), BC6H UF16/SF16, BC7 | ✅ all modes | ✅ (BC6H: mode 11, BC7: mode 6) |
| ETC / EAC | ETC1 (with alpha†), ETC2 RGB / RGBA1 (punchthrough) / RGBA8, EAC R11 / RG11 ± signed | ✅ | ✅ |
| PVRTC | PVRTC1 (with alpha†) 2bpp & 4bpp RGB/RGBA, PVRTC2 2bpp & 4bpp | ✅ | ✅ (PVRTC2: hard-mode) |
| ASTC | LDR + HDR blocks, 2D block sizes 4x4 ... 12x12 | ✅ | ✅ (valid-bitstream baseline) |
| ATC | ATC RGB, RGBA explicit, RGBA interpolated | ✅ | ✅ |
| PICA200 (3DS) | `PICA_ETC1_RGB8` (GPU_ETC1), `PICA_ETC1_RGB8A4` (GPU_ETC1A4) | ✅ | ✅ |
| Raw | RGBA8 passthrough | ✅ | ✅ |

Decode target is 8-bit RGBA (`texc_decode`) or float RGBA (`texc_decode_f32`,
full range for BC6H / ASTC HDR). Encode source is 8-bit RGBA (`texc_encode`)
or float RGBA (`texc_encode_f32`). Non-block-aligned dimensions are handled
(decoders clip, encoders edge-replicate). Encoders favour portability and
determinism over ultimate quality; they produce valid bitstreams everywhere.

† See Alpha-atlas formats below.

### Alpha-atlas formats

Some alpha-less codecs can hold an alpha by storing the image twice in one
double-height texture: RGB on top, the alpha channel as a grayscale map
below. The `*_A_ATLAS` formats reproduce that end to end -
you always pass the **final** (half-height) dimensions:

- `texc_decode` decodes the double-height base image and folds the atlas
  back exactly like the engine (`A = R` of the bottom-half texel).
- `texc_encode` splits your RGBA into the two planes and encodes them as
  one base-codec image.
- `texc_swizzle` / `texc_unswizzle` / `texc_swizzled_size` operate on the
  double-height layout automatically (doubles `HEIGHT` before
  untiling).

### PICA200 (Nintendo 3DS) ETC1

`PICA_ETC1_RGB8` and `PICA_ETC1_RGB8A4` are ordinary ETC1 colour data in
the 3DS's own container, handled transparently:

- the image is stored as **8x8 pixel tiles** in row-major order, each tile
  holding four 4x4 ETC1 blocks in the order (0,0), (4,0), (0,4), (4,4);
- each 64-bit ETC1 block is **byte-reversed** relative to the standard
  big-endian layout;
- `RGB8A4` additionally prefixes every colour block with **8 bytes of
  4-bit alpha**, nibble index `x*4 + y` (ETC1's own pixel order), expanded
  as `(a << 4) | a`.

Block geometry is reported as the 8x8 tile (32 bytes = 4bpp for RGB8,
64 bytes = 8bpp for RGB8A4), so `texc_encoded_size` rounds up to whole
tiles exactly as the hardware stores them. The layout was verified against
devkitPro's tex3ds (which encodes via rg-etc1) and gdkchan/SPICA; note that
SPICA also flips its output vertically as its own convention - that is not
part of the format, so this library keeps its usual top-left origin (use
`texc_flip_y` for the other orientation).

```bash
texc decode -i tex.bin -f PICA_ETC1_RGB8A4 -w 128 -h 128 -o out.png
```

### Encoder options

`texc_encode_ex` takes an optional `texc_encode_options`
(zero-init + `texc_encode_options_init` for defaults; the struct is
versioned by `struct_size` and may grow):

```c
texc_encode_options opts;
texc_encode_options_init(&opts);
opts.alpha_threshold = 200;   /* punchthrough cutoff for BC1 / ETC2_RGBA1 */
texc_encode_ex(TEXC_FORMAT_BC1, rgba, size, w, h, dst, dst_size, &opts);
```

## Unswizzling / reswizzling (console layouts)

Two clearly named directions (swizzle logic credited to Piken / DwayneR):

- **`texc_unswizzle`** - platform-tiled → linear row-major: the direction
  you need **before decoding** console data.
- **`texc_reswizzle`** - linear → platform-tiled: the exact inverse, for
  putting the platform tiling **back** when repacking data for the target
  hardware (e.g. into a G1T). (`texc_swizzle` is the same function under
  its legacy name.)

Every mode's reswizzle→unswizzle roundtrip is verified byte-identical in
the test suite. Supported layouts:

| Mode | Layout |
|---|---|
| `TEXC_SWIZZLE_PS4` / `TEXC_SWIZZLE_PS5` | Orbis/Prospero 8x8 micro-tile Morton |
| `TEXC_SWIZZLE_SWITCH` | Tegra X1 block-linear GOBs (`arg` = block height log2, `0xFFFFFFFF` = auto) |
| `TEXC_SWIZZLE_PSVITA` | GXM Morton / Z-order |
| `TEXC_SWIZZLE_X360` | Xenos macro tiling |
| `TEXC_SWIZZLE_PSP` | 16-byte x 8-row tiles |
| `TEXC_SWIZZLE_3DS` | 8x8 Z-order tiles |
| `TEXC_SWIZZLE_WIIU` | GX2 tiled (`arg` = GX2 swizzle value) |
| `TEXC_SWIZZLE_DX12_64KB` | D3D12 64KB standard swizzle (<64KB data is linear) |

`texc_decode_swizzled` does unswizzle + decode in one call. Container
parsing is intentionally left to the adopting project - this library 
is the conversion core.

## Image utilities

Ported from [tex-decoder](https://github.com/hearhellacopters/tex-decoder)'s
`profiler.ts` and `flipper.ts`:

- **Colour profile converter** - `texc_convert_profile` converts raw pixels
  between 72 profiles (`texc_pixel_profile`: RGBA8/BGRA8 and every channel
  permutation, RGB565, RGBA4, RGBA51, RGB10_A2, signed `*I`, half-float
  `*16F`, float `*32F`, 16/32-bit integer). Semantics match tex-decoder:
  proportional range scaling, a missing source alpha becomes opaque,
  missing colour channels become 0, float 1.0 ≙ integer max.
  `texc_profile_bytes_per_pixel` / `texc_profile_name` give metadata.
- **Flipper** - `texc_flip_y` (row order, tex-decoder `flipImage`) and
  `texc_flip_x` (horizontal mirror), any bytes-per-pixel, in-place capable.
- **Cropper** - `texc_crop` (tex-decoder `cropImage`): copy a rectangle out
  of a raw image with bounds validation.

## Versioning

The version is declared **once**, in `include/tex_codec.h`:

```c
#define TEXC_VERSION_MAJOR 1
#define TEXC_VERSION_MINOR 2
#define TEXC_VERSION_PATCH 0
```

Everything else derives from it, so nothing can drift: CMake parses those
macros for the project/package version, stamps them into the Windows
file-version resource of `tex_codec.dll` and `texc.exe` (visible under
Properties → Details) and into `SOVERSION` on ELF builds, and the test
suite fails if the compiled library, the header or the JS wrapper's
`VERSION` disagree. Bumping a release means editing those three lines and
adding a [CHANGELOG.md](CHANGELOG.md) entry.

New formats, profiles and swizzle modes are only ever **appended** to their
enums, so numeric values stay stable across minor versions - which is what
lets the JS wrapper and other FFI bindings mirror them by number.

Query it at runtime:

| | |
|---|---|
| `texc_version()` | packed `(major << 16) \| (minor << 8) \| patch` |
| `texc_version_string()` | `"1.2.0"` |
| `texc_build_info()` | `tex_codec 1.2.0 (git 3f2a1b8, built Aug 19 2026, MSVC 1944, x64, Release)` |
| `texc version` | the same build line (`texc version --short` prints just `1.2.0`) |
| `await tex.version()` / `tex.buildInfo()` | from JavaScript, plus `tex.checkVersion()` to catch a stale `.wasm` beside a newer wrapper |

Compile-time checks are available too:

```c
#if TEXC_VERSION_NUMBER < TEXC_VERSION_ENCODE(1, 1, 0)
#  error "tex_codec 1.1.0 or newer is required"
#endif
```

CMake consumers can require a version with
`find_package(tex_codec 1.1 REQUIRED)`. The git hash in `texc_build_info()`
is captured at **configure** time - re-run `cmake` to refresh it after
committing.

## Building (native)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

Options: `-DTEXC_BUILD_SHARED=ON` (DLL/.so with exported symbols),
`-DTEXC_BUILD_TESTS=OFF`, `-DTEXC_BUILD_CLI=OFF`.

## Command line tool (`texc`)

The native build also produces `texc` (`build/Release/texc.exe`), a full
front end for the library. `--offset` / `--size` open a window into any
file, so you can point it straight at texture data inside a container
(e.g. a G1T) without extracting it first:

```bash
# decode BC7 data at an offset inside a container straight to PNG
texc decode -i file.g1t --offset 0x1234 --size 0x8000 -f BC7 -w 256 -h 256 -o out.png

# Switch-swizzled ASTC: unswizzle + decode in one step ('auto' block height)
texc decode -i tex.bin -f ASTC_8x8 -w 256 -h 256 --swizzle switch --arg auto -o out.tga

# G1T alpha atlas (final half-height dimensions, alpha folded automatically)
texc decode -i tex.bin -f ETC1_RGB_A_ATLAS -w 128 -h 128 -o out.png

# encode a TGA (dimensions come from the file) and reswizzle for PS4
texc encode -i art.tga -f BC1 --alpha-threshold 200 --swizzle ps4 -o tiled.bc1

# pure layout conversions, clearly named per direction
texc unswizzle -i tiled.bin  -f BC7 -w 256 -h 256 -m switch --arg auto -o linear.bin
texc reswizzle -i linear.bin -f BC7 -w 256 -h 256 -m switch --arg auto -o tiled.bin

# image utilities
texc convert -i in.raw --src-profile RGBA8 --dst-profile BGRA8 -o out.raw
texc flip -i in.raw -w 64 -h 64 --dir y -o out.raw
texc crop -i in.raw -w 64 -h 64 --rect 16,16,32,32 -o out.raw

# sizes without touching data
texc info -f ASTC_8x8 -w 256 -h 256 -m switch --arg auto
```

Output format follows the extension: `.png` / `.tga` for decoded images,
anything else raw bytes (`--f32` writes float RGBA). Encode input is an
uncompressed TGA or raw RGBA8 (with `-w`/`-h`). Discovery commands:
`texc formats`, `texc modes`, `texc profiles`, `texc version`,
`texc help <command>`.

## Building (WebAssembly)

With an activated [emsdk](https://github.com/emscripten-core/emsdk):

```bash
cd wasm && ./build_wasm.sh ../dist
```

or via CMake: `emcmake cmake -S . -B build-wasm && cmake --build build-wasm`.

Produces `tex_codec.js` (MODULARIZE loader, export name `TexCodec`) and
`tex_codec.wasm`, plus copies of the ergonomic wrapper
(`tex_codec_api.mjs` + `tex_codec_api.d.ts`), usable from web, workers, and
Node.

Verify the module with `node wasm/smoke_test.mjs dist` (wrapper roundtrips,
alpha-atlas fold, encoder options, swizzle identity, named exports).

## Usage - C

```c
#include "tex_codec.h"

size_t out_size = texc_decoded_size(width, height);
uint8_t *rgba = texc_alloc(out_size);
int rc = texc_decode(TEXC_FORMAT_BC7, data, data_size,
                     width, height, rgba, out_size);
if (rc != TEXC_OK) fprintf(stderr, "%s\n", texc_result_str(rc));

/* Swizzled console data in one step: */
rc = texc_decode_swizzled(TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_ASTC_8x8,
                          width, height, data, data_size,
                          rgba, out_size, 0xFFFFFFFF /* auto block height */);

/* Encoding: */
size_t enc_size = texc_encoded_size(TEXC_FORMAT_ETC2_RGBA8, width, height);
uint8_t *enc = texc_alloc(enc_size);
rc = texc_encode(TEXC_FORMAT_ETC2_RGBA8, rgba, out_size,
                 width, height, enc, enc_size);

texc_free(rgba); texc_free(enc);
```

## Usage - JavaScript / WASM

The recommended entry point is the wrapper in `wasm/tex_codec_api.mjs`
(TypeScript declarations in `wasm/tex_codec_api.d.ts`; both are static,
hand-maintained files that `build_wasm.sh` copies next to the module). It
gives you real enums, async self-initialising methods, automatic WASM
memory management, and one class per concern:

```js
import { TexCodec, TexFormat, SwizzleMode, PixelProfile,
         SwitchBlockHeightAuto } from "./dist/tex_codec_api.mjs";

const tex = await TexCodec.load();   // finds ./tex_codec.js next to the file

// --- decoding -----------------------------------------------------------
const rgba  = await tex.decoder.decode(TexFormat.BC7, data, w, h);
const hdr   = await tex.decoder.decodeF32(TexFormat.BC6H_UF16, data, w, h);
const fromSwitch = await tex.decoder.decodeSwizzled(
    SwizzleMode.SWITCH, TexFormat.ASTC_8x8, tiled, w, h,
    SwitchBlockHeightAuto);

// G1T alpha atlas - pass the FINAL (half-height) dimensions:
const folded = await tex.decoder.decode(TexFormat.ETC1_RGB_A_ATLAS,
                                        g1tData, w, h);

// --- encoding -----------------------------------------------------------
const bc1 = await tex.encoder.encode(TexFormat.BC1, rgba, w, h,
                                     { alphaThreshold: 200 });
const atlas = await tex.encoder.encode(TexFormat.ETC1_RGB_A_ATLAS,
                                       rgba, w, h);

// --- unswizzling / reswizzling ------------------------------------------
const linear = await tex.swizzler.unswizzle(SwizzleMode.PS4,       // tiled -> linear
                                            TexFormat.BC7, tiled, w, h);
const tiledAgain = await tex.swizzler.reswizzle(SwizzleMode.PS4,   // linear -> tiled
                                                TexFormat.BC7, linear, w, h);
const bytes = await tex.swizzler.swizzledSize(SwizzleMode.SWITCH,
                                              TexFormat.BC1, w, h, 4);

// --- image utilities (tex.image) ----------------------------------------
const bgra    = await tex.image.convertProfile(PixelProfile.RGBA8,
                                               PixelProfile.BGRA8, rgba);
const flipped = await tex.image.flipY(rgba, w, h);          // 4 bpp default
const region  = await tex.image.crop(rgba, w, h, 4, 16, 16, 64, 64);
```

### Per-format / per-mode helper methods

Every class also carries generated named helpers, so call sites don't need
the enums - one per format and one per swizzle mode (fully typed in the
`.d.ts`):

```js
await tex.decoder.decodeBC7(data, w, h);
await tex.decoder.decodeETC1_RGB_A_ATLAS(g1tData, w, h);
await tex.decoder.decodePICA_ETC1_RGB8A4(tex3dsData, w, h);   // 3DS
await tex.decoder.decodeSwizzledSwitch(TexFormat.ASTC_8x8, tiled, w, h,
                                       SwitchBlockHeightAuto);
await tex.encoder.encodeETC2_RGBA8(rgba, w, h);
await tex.encoder.encodeBC1(rgba, w, h, { alphaThreshold: 200 });
await tex.encoder.encodedSizeASTC_4x4(w, h);
await tex.swizzler.unswizzlePS4(TexFormat.BC7, tiled, w, h);   // -> linear
await tex.swizzler.reswizzlePS4(TexFormat.BC7, linear, w, h);  // -> tiled
```

Mode names: `PS4, PS5, Switch, PSVita, X360, PSP, N3DS, WiiU, DX12_64KB`.

Every method throws `TexCodecError` (with a `.code` from `TexResult`) on
failure. In a bundler, pass the module factory yourself:
`TexCodec.load({ factory: (await import("./tex_codec.js")).default })`.

### Raw exports

For callers that manage their own buffers, the full C API is exported on the
module (`tex.module`) - see `TexCodecModule` in `tex_codec_api.d.ts` for
typed signatures:

- **Generic**: `_texc_decode`, `_texc_encode`, `_texc_encode_ex`,
  `_texc_encode_with_options(..., alphaThreshold)`, `_texc_decode_f32`,
  `_texc_unswizzle`, `_texc_swizzle`, `_texc_swizzled_size`,
  `_texc_decode_swizzled`, sizes/metadata, `_malloc` / `_free`.
- **Per format** (all 45): `_texc_decode_bc7(src, srcSize, w, h, dst,
  dstSize)`, `_texc_encode_etc2_rgba8(..., alphaThreshold)`,
  `_texc_encoded_size_astc_12x12(w, h)`, …
- **Per swizzle mode** (ps4, ps5, switch, psvita, x360, psp, n3ds, wiiu,
  dx12_64kb): `_texc_unswizzle_ps4(fmt, w, h, src, srcSize, dst, dstSize,
  arg)` (tiled→linear), `_texc_reswizzle_switch(...)` (linear→tiled;
  `_texc_swizzle_<mode>` is its legacy alias), and
  `_texc_swizzled_size_wiiu(...)`.
- **Image utilities**: `_texc_convert_profile`, `_texc_profile_name`,
  `_texc_profile_bytes_per_pixel`, `_texc_flip_y`, `_texc_flip_x`,
  `_texc_crop`.
- **Allocation helpers**: `_texc_decode_alloc`, `_texc_decode_f32_alloc`,
  `_texc_decode_swizzled_alloc`, `_texc_encode_alloc`,
  `_texc_swizzle_alloc`, with `_texc_last_error()` for the failure reason.

## FFI notes (C# / Rust / Python)

The entire API is `extern "C"`, uses only scalar types and raw pointers, and
never throws or allocates on the caller's behalf (except the documented
`*_alloc` WASM helpers). `texc_format` / `texc_swizzle_mode` are plain ints.
Build with `-DTEXC_BUILD_SHARED=ON` and bind with P/Invoke, `bindgen`, or
`ctypes` directly against `include/tex_codec.h`.

## Layout

```
include/tex_codec.h      public C API
src/tex_codec.cpp        dispatch, validation, buffer management
src/codecs/              bcn.cpp, etc.cpp, astc.cpp, pvrtc.cpp, atc.cpp
src/unswizzle/           platform tiling conversions
wasm/                    Emscripten exports + build script
tests/                   roundtrip + swizzle-identity test suite
```

## Credits / references

- swizzle machinery credit: Piken / DwayneR, github.com/fdwr
- Format specifications: Khronos Data Format Spec (ASTC, ETC2/EAC),
  Microsoft D3D BC1-7 specs, AMD ATC extension, PowerVR PVRTC documentation
- Reference codebases studied: tex-decoder, texture2ddecoder, Basis
  Universal, bimg, ctt
