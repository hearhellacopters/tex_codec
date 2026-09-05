# Changelog

All notable changes to tex_codec. Format based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project
follows [Semantic Versioning](https://semver.org/):

- **MAJOR** - breaking API/ABI change.
- **MINOR** - backward-compatible additions. New formats, profiles and
  swizzle modes are only ever **appended** to their enums, so existing
  numeric values stay valid across minor releases (this matters for FFI and
  for the JavaScript wrapper, which mirrors the enums by number).
- **PATCH** - fixes only.

The version is declared in `include/tex_codec.h` (`TEXC_VERSION_MAJOR` /
`_MINOR` / `_PATCH`) and everything else derives from it - see
[Versioning](README.md#versioning).

## [1.4.0] - 2026-09-04

### Fixed
- **PS Vita raw unswizzling through WebAssembly** returned a wrongly sized
  buffer whenever the 1.3.0 bytes-per-pixel `arg` was used. The C core was
  correct, but the `*_alloc` helpers behind the JS wrapper sized the linear
  side with `texc_encoded_size()`, which knows nothing about the override -
  so a 64x64 24bpp image came back as 16384 bytes (w*h*4) instead of 12288,
  with a garbage tail. Native callers passing their own buffers were
  unaffected.

### Added
- `texc_unswizzled_size(mode, format, w, h, arg)` - the size of the linear
  side of a conversion, honouring the PS Vita raw `arg`. Exposed in
  WebAssembly and as `swizzler.unswizzledSize()` in the JS wrapper. Prefer
  it over `texc_encoded_size()` whenever you pass a non-zero `arg`.
- The WASM smoke test now checks the Vita raw path against the reference
  mapping across 4 sizes x 4 bit depths, so the JS layer is covered too and
  not just the C core.

## [1.3.0] - 2026-09-02

### Fixed
- **PS Vita RAW deswizzle** now matches `DeswizzlePSVitaRaw` ->
  `SwizzleMasterFunction(Block32x32Unswizzle)` byte-for-byte. It previously
  rounded the image up to whole 32x32 tiles, so `texc_swizzled_size`
  over-reported (e.g. 8192 instead of 6144 bytes for 48x32 RGBA8) and real
  G1T data - which is stored unpadded at `w*h*bpp/8` - was rejected with
  `TEXC_ERR_BUFFER_TOO_SMALL`. The raw width is now used directly and
  out-of-range texels are dropped exactly as the reference does. The block
  -compressed Vita path (`convert_morton_psvita_dreamcast`) was already
  correct and is unchanged.

### Added
- `TEXC_SWIZZLE_PSVITA` `arg` selects **bytes per pixel** for raw formats
  (0 = the format's own size), mirroring the reference's `bitsPerPixel`
  parameter - G1T ships 8/16/24/32bpp raw Vita textures, so 24bpp images
  can now be deswizzled by passing 3.
- A regression test compares the port against an independently written copy
  of the reference mapping over 10 sizes x 4 bit depths.

## [1.2.0] - 2026-08-19

### Added
- **PICA200 (Nintendo 3DS) ETC1 formats** `PICA_ETC1_RGB8` (GPU_ETC1,
  4bpp) and `PICA_ETC1_RGB8A4` (GPU_ETC1A4, 8bpp): standard ETC1 colour
  data in the 3DS container - 8x8 pixel tiles holding four 4x4 blocks in
  (0,0),(4,0),(0,4),(4,4) order, each ETC1 block byte-reversed, and for
  RGB8A4 an 8-byte 4-bit alpha plane in front of every colour block
  (nibble index x*4 + y). Block geometry is reported as the 8x8 tile so
  sizes round up to whole tiles like the hardware. Decode, encode, CLI and
  the WebAssembly wrapper all support them; layout verified against
  devkitPro tex3ds (which encodes via rg-etc1) and gdkchan/SPICA.

## [1.1.0] - 2026-08-19

### Added
- **G1T alpha-atlas formats** `ETC1_RGB_A_ATLAS`,
  `PVRTC1_4BPP_RGB_A_ATLAS`, `ETC2_RGB_A_ATLAS` (KTGL `0x6F` / `0x70` /
  `0x71`): base codec stored at double height with the alpha channel as a
  grayscale plane underneath, folded/split automatically on decode/encode
  and handled by the swizzlers.
- **Encoder options** - `texc_encode_options`, `texc_encode_options_init`
  and `texc_encode_ex`, with `alpha_threshold` wired into the BC1 and
  ETC2_RGBA1 punchthrough encoders.
- **Image utilities** ported from tex-decoder: `texc_convert_profile`
  (72 pixel profiles), `texc_flip_y`, `texc_flip_x`, `texc_crop`, plus
  `texc_profile_name` / `texc_profile_bytes_per_pixel`.
- **`texc_reswizzle`** - explicitly named linear → platform-tiled direction
  (alias of `texc_swizzle`, which remains for compatibility).
- **`texc` command line tool** with `decode`, `encode`, `unswizzle`,
  `reswizzle`, `convert`, `flip`, `crop`, `info`, `formats`, `modes`,
  `profiles`, `version` and `help`; `--offset` / `--size` read texture data
  from anywhere inside a container file, PNG/TGA output.
- **WebAssembly**: per-format and per-mode named exports, `*_alloc`
  helpers, and the ergonomic wrapper `wasm/tex_codec_api.mjs` with
  hand-written TypeScript declarations - `TexCodec` plus `decoder`,
  `encoder`, `swizzler` and `image` classes with generated per-format
  helpers (`decodeBC7`, `encodeETC2_RGBA8`, `unswizzlePS4`, …).
- **Version tracking**: `texc_version_string()`, `texc_build_info()`
  (version, git hash, build date, compiler, config), Windows file-version
  resources on `tex_codec.dll` / `texc.exe`, `SOVERSION` on ELF shared
  libraries, a CMake package version file, and wrapper/binary version
  checks (`TexCodec#checkVersion`).

### Changed
- CMake now parses the version out of `include/tex_codec.h` instead of
  declaring its own, so the two can no longer drift.

## [1.0.0] - 2026-08-18

### Added
- Initial release: decode **and** encode for BC1–BC7 (incl. BC4/BC5 SNORM
  and BC6H UF16/SF16), ETC1, ETC2 RGB/RGBA1/RGBA8, EAC R11/RG11 (± signed),
  PVRTC1 2bpp/4bpp RGB/RGBA, PVRTC2 2bpp/4bpp, ASTC across all 14 2D block
  sizes (LDR + HDR decode), ATC RGB/RGBA explicit/interpolated, and RGBA8
  passthrough.
- Platform unswizzling for PS4, PS5, Switch, PS Vita, Xbox 360, PSP, 3DS,
  Wii U (GX2 addrlib subset) and D3D12 64KB layouts, ported from the G1T
  reference headers (swizzle machinery credited to Piken / DwayneR).
- Flat C API (`include/tex_codec.h`), static/shared CMake build, Emscripten
  target, and a roundtrip + swizzle-identity test suite.
