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

## [1.6.0] - 2026-09-17

### Changed
- **PS5 tiling rewritten.** `TEXC_SWIZZLE_PS5` now implements the real
  Prospero layout: AMD GFX10 "standard" swizzle blocks (256 B / 4 KB /
  64 KB) in a row-major raster, defaulting to the 4 KB mode Sony's texture
  tool uses for sampled textures and small G1T PS5 textures use; large
  ones (mip 0 above 64 KB) use the 64 KB mode. The previous code (a RawTex
  Cooker port) approximated 64 KB blocks for everything and read mip 0 at
  the wrong offset, which is why BCn textures came out as scrambled 64px
  tiles. Verified bit-for-bit against Sony's `AgcGpuAddress` host library
  (see `tools/ps5_oracle`), with an SDK-generated known-answer table in
  the test suite and all twelve sample G1T textures decoding correctly.
  **Output of the PS5 mode and `texc_swizzled_size(TEXC_SWIZZLE_PS5, ...)`
  change as a result**; pass `TEXC_PS5_TILE_LEGACY` as `arg` to get the
  old layout.

### Added
- `texc_ps5_tile_mode` (`arg` for the PS5 mode; values match Sony's
  `sce::AgcGpuAddress::TileMode`): `DEFAULT`, `STANDARD_256B`,
  `STANDARD_4KB`, `STANDARD_64KB`, `LEGACY`.
- `texc_ps5_detect_tile_mode`: recovers the tile mode from a texture's
  data size (G1T has no tile-mode field; the 4 KB and 64 KB surfaces have
  different sizes), and `texc_ps5_default_tile_mode`: the observed Koei
  Tecmo choice for the write path (64 KB blocks once mip 0 exceeds 64 KB).
  JS: `detectPS5TileMode()` / `defaultPS5TileMode()`; CLI: `--arg auto`
  on the PS5 mode.
- **Mip-chain surface API**: `texc_get_surface_layout` describes a whole
  tiled surface (mip chain x slices) - total and per-slice size, block
  geometry, and every mip's offset, padded size and mip-tail position -
  and `texc_unswizzle_mip` / `texc_reswizzle_mip` address one mip of one
  slice inside it. Implemented for PS5 (where the mip tail block is stored
  first and mip 0 last, so offsets cannot be derived from linear sizes) and
  for `TEXC_SWIZZLE_NONE`; other modes report `TEXC_ERR_UNSUPPORTED`.
  `texc_surface_layout` / `texc_mip_layout` structs, `TEXC_MAX_MIPS`.
- WASM: `_texc_get_surface_layout`, `_texc_unswizzle_mip`,
  `_texc_reswizzle_mip`, `_texc_unswizzle_mip_alloc`. JS wrapper:
  `PS5TileMode`, `TexSwizzler.surfaceLayout()`, `unswizzleMip()`,
  `reswizzleMip()`, with `SurfaceLayout` / `MipLayout` types.
- CLI: `--mips N [--slices N] --mip N [--slice N]` on `unswizzle` /
  `reswizzle` (whole-surface mode; `--surface <file>` updates an existing
  surface), and `texc info ... --mips N` prints the surface layout.
- `tools/ps5_oracle`: SDK-backed verification matrix and known-answer
  generator (`tests/ps5_kat.h`); the test suite also checks real G1T
  samples when `samples/ps5/textures/` is present.

## [1.5.0] - 2026-09-04

### Added
- **GameCube / Wii GX ("TPL") formats**, ported from Kerilk's `tpl.h` as
  used by Project-G1M's `PLATFORM::RVL` path: `WII_I4`, `WII_I8`, `WII_IA4`,
  `WII_IA8`, `WII_RGB565`, `WII_RGB5A3`, `WII_RGBA8`, `WII_CMPR` (decode +
  encode) and the paletted `WII_C4`, `WII_C8`, `WII_C14X2` (decode). The GX
  tile layout (row-major 32-byte tiles, RGBA8 64; big-endian; even column
  in the high nibble; RGB5A3 dual mode; RGBA8 AR/GB planes; CMPR as four
  DXT1-style sub-blocks with MSB-first selectors) is handled inside the
  formats, and block geometry is reported as the tile.
- `texc_decode_paletted`, `texc_palette_size`, `texc_is_paletted` and
  `texc_palette_format` (`IA8` / `RGB565` / `RGB5A3`, values matching a G1TL
  entry's `WiiPALETTE_TYPE`), plus `TEXC_ERR_NEEDS_PALETTE`. Exposed in
  WebAssembly (`decoder.decodePaletted`, `PaletteFormat`) and in the CLI
  (`--palette`, `--palette-format`).

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
