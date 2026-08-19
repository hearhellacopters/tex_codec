/**
 * tex_codec_api.d.ts - TypeScript declarations for the tex_codec WASM
 * module (dist/tex_codec.js) and the ergonomic wrapper
 * (tex_codec_api.mjs). Hand-maintained; keep in sync with
 * include/tex_codec.h and wasm/exports.cpp.
 */

/* ------------------------------------------------------------------ enums */

/** Texture formats; values match `texc_format` in include/tex_codec.h. */
export declare const TexFormat: Readonly<{
  INVALID: 0; RGBA8: 1;
  BC1: 2; BC2: 3; BC3: 4; BC4: 5; BC4_SNORM: 6; BC5: 7; BC5_SNORM: 8;
  BC6H_UF16: 9; BC6H_SF16: 10; BC7: 11;
  ETC1_RGB: 12; ETC2_RGB: 13; ETC2_RGBA1: 14; ETC2_RGBA8: 15;
  EAC_R11: 16; EAC_R11_SIGNED: 17; EAC_RG11: 18; EAC_RG11_SIGNED: 19;
  PVRTC1_2BPP_RGB: 20; PVRTC1_2BPP_RGBA: 21;
  PVRTC1_4BPP_RGB: 22; PVRTC1_4BPP_RGBA: 23;
  PVRTC2_2BPP: 24; PVRTC2_4BPP: 25;
  ASTC_4x4: 26; ASTC_5x4: 27; ASTC_5x5: 28; ASTC_6x5: 29; ASTC_6x6: 30;
  ASTC_8x5: 31; ASTC_8x6: 32; ASTC_8x8: 33; ASTC_10x5: 34; ASTC_10x6: 35;
  ASTC_10x8: 36; ASTC_10x10: 37; ASTC_12x10: 38; ASTC_12x12: 39;
  ATC_RGB: 40; ATC_RGBA_EXPLICIT: 41; ATC_RGBA_INTERPOLATED: 42;
  /** G1T alpha-atlas formats: base codec at double height, alpha folded
   *  from the grayscale bottom half. Pass FINAL (half-height) dimensions. */
  ETC1_RGB_A_ATLAS: 43; PVRTC1_4BPP_RGB_A_ATLAS: 44; ETC2_RGB_A_ATLAS: 45;
}>;
export type TexFormatValue = (typeof TexFormat)[keyof typeof TexFormat];

/** Platform swizzle layouts; values match `texc_swizzle_mode`. */
export declare const SwizzleMode: Readonly<{
  NONE: 0; PS4: 1; PS5: 2; SWITCH: 3; PSVITA: 4; X360: 5; PSP: 6; N3DS: 7;
  WIIU: 8; DX12_64KB: 9;
}>;
export type SwizzleModeValue = (typeof SwizzleMode)[keyof typeof SwizzleMode];

/** `arg` value that makes SWITCH auto-select the GOB block height. */
export declare const SwitchBlockHeightAuto: 0xffffffff;

/** Raw pixel layouts (tex-decoder COLOR_PROFILE); values match
 *  `texc_pixel_profile`. Suffix: none = unsigned, I = signed,
 *  16F = half float, 32F = float. */
export declare const PixelProfile: Readonly<{
  INVALID: 0;
  A8: 1; R8: 2; G8: 3; B8: 4;
  RG8: 5; RB8: 6; GR8: 7; GB8: 8; BR8: 9; BG8: 10;
  RGB8: 11; RBG8: 12; GRB8: 13; GBR8: 14; BRG8: 15; BGR8: 16;
  ARGB8: 17; ARBG8: 18; AGRB8: 19; AGBR8: 20; ABRG8: 21; ABGR8: 22;
  RGBA8: 23; RBGA8: 24; GRBA8: 25; GBRA8: 26; BRGA8: 27; BGRA8: 28;
  RGB565: 29; BGR565: 30; RGBA4: 31; RGBA51: 32;
  RGB10_A2: 33; RGB10_A2I: 34;
  A8I: 35; R8I: 36; RG8I: 37; RGB8I: 38; RGBA8I: 39; ARGB8I: 40;
  BGR8I: 41; BGRA8I: 42; ABGR8I: 43;
  A16F: 44; R16F: 45; RG16F: 46; RGB16F: 47; RGBA16F: 48; ARGB16F: 49;
  R16: 50; RG16: 51; RGB16: 52; RGBA16: 53;
  A16I: 54; R16I: 55; RG16I: 56; RGB16I: 57; RGBA16I: 58;
  A32F: 59; R32F: 60; RG32F: 61; RGB32F: 62; RGBA32F: 63;
  A32: 64; R32: 65; RG32: 66; RGB32: 67; RGBA32: 68;
  R32I: 69; RG32I: 70; RGB32I: 71; RGBA32I: 72;
}>;
export type PixelProfileValue =
    (typeof PixelProfile)[keyof typeof PixelProfile];

/** Result codes returned by the C core (negative = failure). */
export declare const TexResult: Readonly<{
  OK: 0; INVALID_ARG: -1; BUFFER_TOO_SMALL: -2; UNSUPPORTED: -3;
  BAD_DATA: -4; OUT_OF_MEMORY: -5; BAD_DIMENSIONS: -6;
}>;
export type TexResultValue = (typeof TexResult)[keyof typeof TexResult];

/* -------------------------------------------------------- raw WASM module */

/** Byte pointer into WASM linear memory (0 = NULL). */
export type Ptr = number;

/**
 * The initialised Emscripten module: `default export of tex_codec.js` is a
 * factory returning `Promise<TexCodecModule>`.
 *
 * Pointer-based calls follow the C API exactly (see include/tex_codec.h for
 * full semantics); allocate with `_malloc`, copy via `HEAPU8`, free with
 * `_free` / `_texc_free`.
 */
export interface TexCodecModule {
  /* Emscripten runtime */
  HEAPU8: Uint8Array;
  HEAPF32: Float32Array;
  getValue(ptr: Ptr, type: "i8" | "i16" | "i32" | "float" | "double"): number;
  setValue(ptr: Ptr, value: number,
           type: "i8" | "i16" | "i32" | "float" | "double"): void;
  UTF8ToString(ptr: Ptr): string;
  ccall(name: string, returnType: string | null, argTypes: string[],
        args: unknown[]): unknown;
  cwrap(name: string, returnType: string | null,
        argTypes: string[]): (...args: unknown[]) => unknown;
  _malloc(size: number): Ptr;
  _free(ptr: Ptr): void;

  /* ------------------------------------------------ generic C API ------ */
  _texc_version(): number;
  _texc_format_name(format: number): Ptr;           /* const char* */
  _texc_result_str(result: number): Ptr;            /* const char* */
  _texc_block_dims(format: number, widthOut: Ptr, heightOut: Ptr,
                   bytesOut: Ptr): number;
  _texc_encoded_size(format: number, width: number, height: number): number;
  _texc_decoded_size(width: number, height: number): number;
  _texc_can_decode(format: number): number;
  _texc_can_encode(format: number): number;
  _texc_decode(format: number, src: Ptr, srcSize: number,
               width: number, height: number, dst: Ptr,
               dstSize: number): number;
  _texc_decode_f32(format: number, src: Ptr, srcSize: number,
                   width: number, height: number, dst: Ptr,
                   dstSize: number): number;
  _texc_encode(format: number, src: Ptr, srcSize: number,
               width: number, height: number, dst: Ptr,
               dstSize: number): number;
  /** opts: pointer to a texc_encode_options struct, or 0. */
  _texc_encode_ex(format: number, src: Ptr, srcSize: number,
                  width: number, height: number, dst: Ptr, dstSize: number,
                  opts: Ptr): number;
  _texc_encode_f32(format: number, src: Ptr, srcSize: number,
                   width: number, height: number, dst: Ptr,
                   dstSize: number): number;
  /** texc_encode with the alpha threshold as a scalar (0-256, 128 = default;
   *  affects BC1 punchthrough and ETC2_RGBA1). */
  _texc_encode_with_options(format: number, src: Ptr, srcSize: number,
                            width: number, height: number, dst: Ptr,
                            dstSize: number, alphaThreshold: number): number;
  _texc_swizzled_size(mode: number, format: number, width: number,
                      height: number, arg: number): number;
  _texc_unswizzle(mode: number, format: number, width: number, height: number,
                  src: Ptr, srcSize: number, dst: Ptr, dstSize: number,
                  arg: number): number;
  _texc_swizzle(mode: number, format: number, width: number, height: number,
                src: Ptr, srcSize: number, dst: Ptr, dstSize: number,
                arg: number): number;
  _texc_decode_swizzled(mode: number, format: number, width: number,
                        height: number, src: Ptr, srcSize: number, dst: Ptr,
                        dstSize: number, arg: number): number;
  /** RESWIZZLE (linear -> tiled): alias of _texc_swizzle with an
   *  unambiguous name. */
  _texc_reswizzle(mode: number, format: number, width: number, height: number,
                  src: Ptr, srcSize: number, dst: Ptr, dstSize: number,
                  arg: number): number;
  _texc_alloc(size: number): Ptr;
  _texc_free(ptr: Ptr): void;

  /* --------------------------------------------- image utilities ------- */
  _texc_profile_name(profile: number): Ptr;         /* const char* */
  _texc_profile_bytes_per_pixel(profile: number): number;
  /** pixelCount 0 = derive from srcSize. */
  _texc_convert_profile(srcProfile: number, src: Ptr, srcSize: number,
                        dstProfile: number, dst: Ptr, dstSize: number,
                        pixelCount: number): number;
  /** dst may equal src for an in-place flip. */
  _texc_flip_y(src: Ptr, srcSize: number, dst: Ptr, dstSize: number,
               width: number, height: number, bytesPerPixel: number): number;
  _texc_flip_x(src: Ptr, srcSize: number, dst: Ptr, dstSize: number,
               width: number, height: number, bytesPerPixel: number): number;
  _texc_crop(src: Ptr, srcSize: number, width: number, height: number,
             bytesPerPixel: number, x: number, y: number, cropWidth: number,
             cropHeight: number, dst: Ptr, dstSize: number): number;

  /* --------------------------------------- allocation-style helpers ---- */
  /** Result code of the most recent *_alloc call that returned NULL. */
  _texc_last_error(): number;
  /** Returns a malloc'd RGBA8 buffer (w*h*4 bytes) or 0; free with
   *  _texc_free. */
  _texc_decode_alloc(format: number, src: Ptr, srcSize: number,
                     width: number, height: number): Ptr;
  /** Returns a malloc'd float RGBA buffer (w*h*16 bytes) or 0. */
  _texc_decode_f32_alloc(format: number, src: Ptr, srcSize: number,
                         width: number, height: number): Ptr;
  _texc_decode_swizzled_alloc(mode: number, format: number, src: Ptr,
                              srcSize: number, width: number, height: number,
                              arg: number): Ptr;
  /** Returns a malloc'd compressed buffer or 0; byte count written to
   *  outSizePtr (4 bytes) when non-zero. */
  _texc_encode_alloc(format: number, srcRgba: Ptr, srcSize: number,
                     width: number, height: number, outSizePtr: Ptr): Ptr;
  /** toLinear != 0: tiled -> linear; toLinear == 0: linear -> tiled. */
  _texc_swizzle_alloc(mode: number, format: number, src: Ptr,
                      srcSize: number, width: number, height: number,
                      arg: number, toLinear: number, outSizePtr: Ptr): Ptr;

  /* -------------------------------------- per-format named wrappers ----
   * For every format suffix below there are three exports:
   *   _texc_decode_<s>(src, srcSize, w, h, dst, dstSize): number
   *   _texc_encode_<s>(src, srcSize, w, h, dst, dstSize,
   *                    alphaThreshold): number
   *   _texc_encoded_size_<s>(w, h): number
   * Suffixes: rgba8, bc1, bc2, bc3, bc4, bc4_snorm, bc5, bc5_snorm,
   * bc6h_uf16, bc6h_sf16, bc7, etc1_rgb, etc2_rgb, etc2_rgba1, etc2_rgba8,
   * eac_r11, eac_r11_signed, eac_rg11, eac_rg11_signed, pvrtc1_2bpp_rgb,
   * pvrtc1_2bpp_rgba, pvrtc1_4bpp_rgb, pvrtc1_4bpp_rgba, pvrtc2_2bpp,
   * pvrtc2_4bpp, astc_4x4 ... astc_12x12, atc_rgb, atc_rgba_explicit,
   * atc_rgba_interpolated, etc1_rgb_a_atlas, pvrtc1_4bpp_rgb_a_atlas,
   * etc2_rgb_a_atlas.
   *
   * -------------------------------------- per-mode swizzle wrappers ----
   * For every mode suffix (ps4, ps5, switch, psvita, x360, psp, n3ds, wiiu,
   * dx12_64kb):
   *   _texc_unswizzle_<m>(format, w, h, src, srcSize, dst, dstSize,
   *                       arg): number       // tiled -> linear
   *   _texc_reswizzle_<m>(format, w, h, src, srcSize, dst, dstSize,
   *                       arg): number       // linear -> tiled
   *   _texc_swizzle_<m>(...): number         // legacy alias of reswizzle
   *   _texc_swizzled_size_<m>(format, w, h, arg): number
   */
  [key: `_texc_decode_${string}`]: (...args: number[]) => number;
  [key: `_texc_encode_${string}`]: (...args: number[]) => number;
  [key: `_texc_encoded_size_${string}`]: (...args: number[]) => number;
  [key: `_texc_unswizzle_${string}`]: (...args: number[]) => number;
  [key: `_texc_reswizzle_${string}`]: (...args: number[]) => number;
  [key: `_texc_swizzle_${string}`]: (...args: number[]) => number;
  [key: `_texc_swizzled_size_${string}`]: (...args: number[]) => number;
}

/** Factory exported (default) by dist/tex_codec.js. */
export type TexCodecModuleFactory =
    (overrides?: Record<string, unknown>) => Promise<TexCodecModule>;

/* ---------------------------------------------------------------- wrapper */

/** Error thrown by every wrapper method on a non-OK result. */
export declare class TexCodecError extends Error {
  readonly name: "TexCodecError";
  /** A {@link TexResult} value. */
  readonly code: number;
  constructor(operation: string, code: number, detail?: string);
}

export interface EncodeOptions {
  /** 0-256, default 128 - pixels with alpha below it become transparent in
   *  punchthrough formats (BC1 3-colour mode, ETC2_RGBA1). */
  alphaThreshold?: number;
}

/* Per-format / per-mode helper method names, generated at import time in
 * tex_codec_api.mjs. One method per TexFormat key (minus INVALID), e.g.
 * decodeBC1 / encodeETC2_RGBA8 / encodedSizeASTC_4x4, and one per swizzle
 * mode with prettified names (PS4, PS5, Switch, PSVita, X360, PSP, N3DS,
 * WiiU, DX12_64KB), e.g. unswizzlePS4 / reswizzleSwitch. */
export type FormatKey = Exclude<keyof typeof TexFormat, "INVALID">;
export type SwizzleModeName =
    "PS4" | "PS5" | "Switch" | "PSVita" | "X360" | "PSP" | "N3DS" | "WiiU" |
    "DX12_64KB";

type PerFormatDecode = {
  [K in FormatKey as `decode${K}`]:
      (data: Uint8Array, width: number, height: number) => Promise<Uint8Array>;
};
type PerFormatDecodeF32 = {
  [K in FormatKey as `decodeF32${K}`]:
      (data: Uint8Array, width: number,
       height: number) => Promise<Float32Array>;
};
type PerModeDecodeSwizzled = {
  [M in SwizzleModeName as `decodeSwizzled${M}`]:
      (format: TexFormatValue | number, data: Uint8Array, width: number,
       height: number, arg?: number) => Promise<Uint8Array>;
};
type PerFormatEncode = {
  [K in FormatKey as `encode${K}`]:
      (rgba: Uint8Array, width: number, height: number,
       options?: EncodeOptions) => Promise<Uint8Array>;
};
type PerFormatEncodedSize = {
  [K in FormatKey as `encodedSize${K}`]:
      (width: number, height: number) => Promise<number>;
};
type PerModeUnswizzle = {
  [M in SwizzleModeName as `unswizzle${M}`]:
      (format: TexFormatValue | number, data: Uint8Array, width: number,
       height: number, arg?: number) => Promise<Uint8Array>;
};
type PerModeReswizzle = {
  [M in SwizzleModeName as `reswizzle${M}`]:
      (format: TexFormatValue | number, data: Uint8Array, width: number,
       height: number, arg?: number) => Promise<Uint8Array>;
};
type PerModeSwizzledSize = {
  [M in SwizzleModeName as `swizzledSize${M}`]:
      (format: TexFormatValue | number, width: number, height: number,
       arg?: number) => Promise<number>;
};

/** Decoding: compressed data -> RGBA pixels. */
export declare class TexDecoder {
  decode(format: TexFormatValue | number, data: Uint8Array,
         width: number, height: number): Promise<Uint8Array>;
  decodeF32(format: TexFormatValue | number, data: Uint8Array,
            width: number, height: number): Promise<Float32Array>;
  decodeSwizzled(mode: SwizzleModeValue | number,
                 format: TexFormatValue | number, data: Uint8Array,
                 width: number, height: number,
                 arg?: number): Promise<Uint8Array>;
}
export interface TexDecoder
    extends PerFormatDecode, PerFormatDecodeF32, PerModeDecodeSwizzled {}

/** Encoding: RGBA pixels -> compressed data. */
export declare class TexEncoder {
  encodedSize(format: TexFormatValue | number, width: number,
              height: number): Promise<number>;
  encode(format: TexFormatValue | number, rgba: Uint8Array,
         width: number, height: number,
         options?: EncodeOptions): Promise<Uint8Array>;
}
export interface TexEncoder
    extends PerFormatEncode, PerFormatEncodedSize {}

/** Platform tiling conversions.
 *  unswizzle = tiled -> linear (before decoding);
 *  reswizzle = linear -> tiled (putting the platform tiling back). */
export declare class TexSwizzler {
  swizzledSize(mode: SwizzleModeValue | number,
               format: TexFormatValue | number, width: number, height: number,
               arg?: number): Promise<number>;
  /** UNSWIZZLE: platform-tiled -> linear row-major. */
  unswizzle(mode: SwizzleModeValue | number, format: TexFormatValue | number,
            data: Uint8Array, width: number, height: number,
            arg?: number): Promise<Uint8Array>;
  /** RESWIZZLE: linear row-major -> platform-tiled (inverse of unswizzle). */
  reswizzle(mode: SwizzleModeValue | number, format: TexFormatValue | number,
            data: Uint8Array, width: number, height: number,
            arg?: number): Promise<Uint8Array>;
  /** @deprecated legacy alias of reswizzle */
  swizzle(mode: SwizzleModeValue | number, format: TexFormatValue | number,
          data: Uint8Array, width: number, height: number,
          arg?: number): Promise<Uint8Array>;
}
export interface TexSwizzler
    extends PerModeUnswizzle, PerModeReswizzle, PerModeSwizzledSize {}

/** Raw-pixel utilities: colour profile conversion, flipping, cropping
 *  (tex-decoder profiler.ts / flipper.ts ports). */
export declare class TexImage {
  profileBytesPerPixel(profile: PixelProfileValue | number): Promise<number>;
  /** Proportional range scaling; missing source alpha -> opaque, missing
   *  colour channels -> 0; float profiles treat 1.0 as integer max. */
  convertProfile(srcProfile: PixelProfileValue | number,
                 dstProfile: PixelProfileValue | number, data: Uint8Array,
                 pixelCount?: number): Promise<Uint8Array>;
  flipY(data: Uint8Array, width: number, height: number,
        bytesPerPixel?: number): Promise<Uint8Array>;
  flipX(data: Uint8Array, width: number, height: number,
        bytesPerPixel?: number): Promise<Uint8Array>;
  crop(data: Uint8Array, width: number, height: number, bytesPerPixel: number,
       x: number, y: number, cropWidth: number,
       cropHeight: number): Promise<Uint8Array>;
}

export interface TexCodecLoadOptions {
  /** The factory exported by tex_codec.js, if you import it yourself. */
  factory?: TexCodecModuleFactory;
  /** URL of tex_codec.js to import dynamically
   *  (default: "./tex_codec.js" next to tex_codec_api.mjs). */
  moduleUrl?: string | URL;
}

/** Entry point; wraps one initialised WASM module. */
export declare class TexCodec {
  /** Raw Emscripten module - every C export is reachable here. */
  readonly module: TexCodecModule;
  readonly decoder: TexDecoder;
  readonly encoder: TexEncoder;
  readonly swizzler: TexSwizzler;
  readonly image: TexImage;

  static load(options?: TexCodecLoadOptions): Promise<TexCodec>;

  version(): Promise<{ major: number; minor: number; patch: number }>;
  formatName(format: TexFormatValue | number): Promise<string | null>;
  blockDims(format: TexFormatValue | number):
      Promise<{ width: number; height: number; bytes: number }>;
}
