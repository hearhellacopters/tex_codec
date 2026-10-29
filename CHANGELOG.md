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

## [1.8.0] - 2026-10-28

### Changed
- **`TEXC_SWIZZLE_DX12_64KB` rewritten** from Rise of the Ronin's own CPU
  upload path. KTGL stores a 64 KB-tiled PC texture as row-major tiles of
  the D3D12 standard shape for the element size (256x256 elements at 1
  byte, 256x128 at 2, 128x128 at 4, 128x64 at 8, 64x64 at 16; BC1/BC4 =
  512x256 pixels, BC2/3/5/6H/7 = 256x256), each tile stored in one piece
  row by row, edge tiles only as wide or tall as what is left. The stored
  size is therefore always the linear size, and a mip at most one tile
  wide is plain linear. The old mode padded edge tiles, treated "64 KB or
  less" as the linear case and used 128x128 tiles for every raw element
  size. **Output and `texc_swizzled_size` change for this mode.**
- **X360 byte order now depends on the element size**, following the
  D3DFORMATs in Dynasty Warriors 8's G1T loader: 8in16 for DXT and 2- or
  8-byte texels, 8in32 for 4- and 16-byte texels, none for 1-byte texels.
  Every format used to be swapped 8in16, which scrambled 32-bit texels
  such as RGBA8. **RGBA8 X360 output changes**; pass
  `TEXC_X360_ENDIAN_8IN16` for the old order.

### Added
- `texc_x360_endian` (`TEXC_X360_ENDIAN_AUTO` / `NONE` / `8IN16` / `8IN32`
  / `16IN32`), OR'ed into the X360 `arg` next to the pitch override
  (`TEXC_X360_PITCH_MASK`, `TEXC_X360_ENDIAN_MASK`).
- `TEXC_X360_LOADER_ROWS`: the X360 tiled side is the rows a G1T stores
  (KTGL reads each one into the tiled surface at the surface pitch and never
  writes the rest) instead of the whole surface. Unswizzle accepts shorter
  input and reads the missing tail as zeros; the CLI prints how many bytes
  were missing.
- JS wrapper: `X360Endian`, `X360LoaderRows`. CLI: the new fields are
  listed under `texc modes`.
- **Wii U mip chains**: `texc_get_surface_layout`, `texc_unswizzle_mip` and
  `texc_reswizzle_mip` accept `TEXC_SWIZZLE_WIIU` (JS `surfaceLayout` /
  `unswizzleMip` / `reswizzleMip`, CLI `-m wiiu --mips N`). From Hyrule
  Warriors' loader, which hands GX2 the G1T bytes with tile mode DEFAULT:
  mip 0 stays 2D_TILED_THIN1 at any size, and a smaller mip narrower than
  a macro tile is 1D_TILED_THIN1, padded to 8x8 micro tiles, each mip at its
  addrlib base alignment after the last. Payload sizes of real textures
  match exactly, and each 1D mip decodes to its parent mip scaled down.
  These small mips were what the single-mip `WIIU` mode could not read.
  Single-mip arrays and cube maps are supported too, with addrlib's
  per-slice bank rotation (the sample cube maps decode smoother with it
  than face by face); several mips of several slices return
  `TEXC_ERR_UNSUPPORTED`, because GX2 stores each level's slices together.
- `tools/ps4_oracle`: checks the PS4 mode against Sony's GpuAddress code,
  run as a black box from a KTGL PS4 eboot (natively, through a
  Windows-to-SysV thunk, since the code uses AVX). `verify` confirms
  `Thin_1dThin` 8x8 Morton micro-tiles for every element size, the pitch
  and size rules for `Thin_1dThin` and `Display_LinearAligned`, and that
  `texc reswizzle -m ps4` matches Sony byte for byte (BC1/3/4/7, RGBA8).
  Nothing from the eboot is in the repository.
- Tests: DX12 64KB against a transcription of Ronin's upload loop
  (RGBA8, BC1, BC4, BC7 at sizes up to 2049x2049); X360 byte order per
  mode, stored-rows placement and short input; matching WASM smoke checks.

## [1.7.0] - 2026-10-27

### Added
- **Switch 2 (NVIDIA Orin) block-linear layout**: `TEXC_SWIZZLE_SWITCH2`
  (G1T platform `0x18`). It shares everything with `TEXC_SWIZZLE_SWITCH`
  except the byte order inside each 64-byte x 8-row GOB, where two address
  bits are swapped: the same `arg` (block height log2 or `0xFFFFFFFF` for
  auto), block-height rules and tiled sizes. Taken from the `isOrin` path
  of ScanMountGoat's `tegra_swizzle`, and checked against that library in
  `tests/switch2_kat.h` for both GPUs (24 cases over RGBA8, BC1, BC7 and
  ASTC 8x8 at even and odd sizes).
- WASM: `_texc_unswizzle_switch2`, `_texc_reswizzle_switch2`,
  `_texc_swizzle_switch2`, `_texc_swizzled_size_switch2`. JS wrapper:
  `SwizzleMode.SWITCH2` and the generated `unswizzleSwitch2`,
  `reswizzleSwitch2`, `swizzledSizeSwitch2`, `decodeSwizzledSwitch2`
  helpers, typed in the `.d.ts`. CLI: `-m switch2` (alias `orin`).

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
