/**
 * tex_codec_api.mjs - ergonomic JavaScript wrapper around the tex_codec
 * WebAssembly module.
 *
 * Everything here is plain ESM with JSDoc types (works from TypeScript via
 * `allowJs`/checkJs or the bundled tex_codec_api.d.ts - both files are
 * static and hand-maintained, nothing is generated).
 *
 * Quick start:
 * ```js
 * import { TexCodec, TexFormat, SwizzleMode } from "./tex_codec_api.mjs";
 *
 * const tex = await TexCodec.load();          // loads ./tex_codec.js next to this file
 *
 * const rgba = await tex.decoder.decode(TexFormat.BC7, data, w, h);
 * const enc  = await tex.encoder.encode(TexFormat.ETC2_RGBA8, rgba, w, h);
 * const lin  = await tex.swizzler.unswizzle(SwizzleMode.SWITCH,
 *                                           TexFormat.ASTC_8x8, tiled, w, h,
 *                                           SwitchBlockHeightAuto);
 * ```
 *
 * Design notes:
 * - Every method is async and safe to call before `TexCodec.load()` - the
 *   first call performs (or awaits) module initialisation itself.
 * - All WASM memory management (malloc / copy-in / copy-out / free) is
 *   internal; inputs are plain typed arrays, outputs are fresh typed arrays
 *   detached from the WASM heap (safe across memory growth).
 * - The raw Emscripten exports remain available as `tex.module` (e.g.
 *   `tex.module._texc_decode_bc7(...)`) for callers that manage their own
 *   buffers; see tex_codec_api.d.ts `TexCodecModule` for their signatures.
 */

/* ---------------------------------------------------------------- version */

/**
 * Version of this JavaScript wrapper. Must match the `TEXC_VERSION_*`
 * macros in include/tex_codec.h - {@link TexCodec#checkVersion} compares it
 * against the loaded WASM binary so a stale `tex_codec.wasm` next to a new
 * wrapper (or vice versa) is caught instead of silently misbehaving.
 * @type {string}
 */
export const VERSION = "1.4.0";

/* ------------------------------------------------------------------ enums */

/**
 * Texture formats. Values MUST match the declaration order of `texc_format`
 * in include/tex_codec.h - do not renumber (new formats are only ever
 * appended there, so these values are stable).
 * @readonly @enum {number}
 */
export const TexFormat = Object.freeze({
  INVALID: 0,
  RGBA8: 1,
  BC1: 2, BC2: 3, BC3: 4,
  BC4: 5, BC4_SNORM: 6, BC5: 7, BC5_SNORM: 8,
  BC6H_UF16: 9, BC6H_SF16: 10, BC7: 11,
  ETC1_RGB: 12, ETC2_RGB: 13, ETC2_RGBA1: 14, ETC2_RGBA8: 15,
  EAC_R11: 16, EAC_R11_SIGNED: 17, EAC_RG11: 18, EAC_RG11_SIGNED: 19,
  PVRTC1_2BPP_RGB: 20, PVRTC1_2BPP_RGBA: 21,
  PVRTC1_4BPP_RGB: 22, PVRTC1_4BPP_RGBA: 23,
  PVRTC2_2BPP: 24, PVRTC2_4BPP: 25,
  ASTC_4x4: 26, ASTC_5x4: 27, ASTC_5x5: 28, ASTC_6x5: 29, ASTC_6x6: 30,
  ASTC_8x5: 31, ASTC_8x6: 32, ASTC_8x8: 33, ASTC_10x5: 34, ASTC_10x6: 35,
  ASTC_10x8: 36, ASTC_10x10: 37, ASTC_12x10: 38, ASTC_12x12: 39,
  ATC_RGB: 40, ATC_RGBA_EXPLICIT: 41, ATC_RGBA_INTERPOLATED: 42,
  /** G1T alpha-atlas formats (KTGL ETC1RGBETC1A / PVRT4RGBPVRT4A /
   *  ETC2RGBA8): base codec at double height, alpha folded from the
   *  grayscale bottom half. Pass the FINAL image dimensions. */
  ETC1_RGB_A_ATLAS: 43, PVRTC1_4BPP_RGB_A_ATLAS: 44, ETC2_RGB_A_ATLAS: 45,
  /** PICA200 (3DS) ETC1: 8x8 tiles of four 4x4 ETC1 blocks, blocks
   *  byte-reversed; RGB8A4 prefixes each block with 8 bytes of 4-bit
   *  alpha. Sizes round up to whole tiles. */
  PICA_ETC1_RGB8: 46, PICA_ETC1_RGB8A4: 47,
});

/**
 * Platform swizzle layouts. Values match `texc_swizzle_mode` in
 * include/tex_codec.h.
 * @readonly @enum {number}
 */
export const SwizzleMode = Object.freeze({
  NONE: 0,
  PS4: 1,
  PS5: 2,
  /** Tegra X1 block-linear GOBs; `arg` = log2 block height (0-5) or
   *  {@link SwitchBlockHeightAuto}. */
  SWITCH: 3,
  /** GXM Morton/Z-order. For RAW (non-block) formats `arg` is bytes per
   *  pixel (0 = the format's own size); G1T ships 8/16/24/32bpp raw Vita
   *  textures, so pass 3 for a 24bpp image. */
  PSVITA: 4,
  /** `arg` = texel byte pitch override, 0 = default. */
  X360: 5,
  PSP: 6,
  N3DS: 7,
  /** `arg` = GX2 swizzle value from the texture header (usually 0). */
  WIIU: 8,
  /** Data of 64KB or less is stored linearly (pass-through). */
  DX12_64KB: 9,
});

/** `arg` value that makes SWITCH auto-select the GOB block height from the
 *  mip height, like the hardware. */
export const SwitchBlockHeightAuto = 0xFFFFFFFF;

/**
 * Raw pixel layouts for {@link TexImage#convertProfile} (ported from
 * tex-decoder's COLOR_PROFILE). The name encodes channel order and bits;
 * the suffix encodes the value type: none = unsigned, I = signed,
 * 16F = half float, 32F = float. Values match `texc_pixel_profile` in
 * include/tex_codec.h.
 * @readonly @enum {number}
 */
export const PixelProfile = Object.freeze({
  INVALID: 0,
  A8: 1, R8: 2, G8: 3, B8: 4,
  RG8: 5, RB8: 6, GR8: 7, GB8: 8, BR8: 9, BG8: 10,
  RGB8: 11, RBG8: 12, GRB8: 13, GBR8: 14, BRG8: 15, BGR8: 16,
  ARGB8: 17, ARBG8: 18, AGRB8: 19, AGBR8: 20, ABRG8: 21, ABGR8: 22,
  RGBA8: 23, RBGA8: 24, GRBA8: 25, GBRA8: 26, BRGA8: 27, BGRA8: 28,
  RGB565: 29, BGR565: 30, RGBA4: 31, RGBA51: 32,
  RGB10_A2: 33, RGB10_A2I: 34,
  A8I: 35, R8I: 36, RG8I: 37, RGB8I: 38, RGBA8I: 39, ARGB8I: 40,
  BGR8I: 41, BGRA8I: 42, ABGR8I: 43,
  A16F: 44, R16F: 45, RG16F: 46, RGB16F: 47, RGBA16F: 48, ARGB16F: 49,
  R16: 50, RG16: 51, RGB16: 52, RGBA16: 53,
  A16I: 54, R16I: 55, RG16I: 56, RGB16I: 57, RGBA16I: 58,
  A32F: 59, R32F: 60, RG32F: 61, RGB32F: 62, RGBA32F: 63,
  A32: 64, R32: 65, RG32: 66, RGB32: 67, RGBA32: 68,
  R32I: 69, RG32I: 70, RGB32I: 71, RGBA32I: 72,
});

/**
 * Result codes returned by the C core (negative = failure).
 * @readonly @enum {number}
 */
export const TexResult = Object.freeze({
  OK: 0,
  INVALID_ARG: -1,
  BUFFER_TOO_SMALL: -2,
  UNSUPPORTED: -3,
  BAD_DATA: -4,
  OUT_OF_MEMORY: -5,
  BAD_DIMENSIONS: -6,
});

/* ------------------------------------------------------------------ error */

/** Error thrown by every API method on a non-OK result code. */
export class TexCodecError extends Error {
  /**
   * @param {string} operation what was attempted, e.g. "decode(BC7)"
   * @param {number} code a TexResult value
   * @param {string} [detail] the C library's message for the code
   */
  constructor(operation, code, detail) {
    super(`tex_codec ${operation} failed: ${detail ?? "error"} (code ${code})`);
    this.name = "TexCodecError";
    /** @type {number} a TexResult value */
    this.code = code;
  }
}

/* ------------------------------------------------------------- internals */

/** @typedef {import("./tex_codec_api.d.ts").TexCodecModule} TexCodecModule */

/** @type {Promise<TexCodecModule> | null} */
let defaultModulePromise = null;

/**
 * Copy bytes into WASM memory, run `fn(ptr)`, always free.
 * @template T
 * @param {TexCodecModule} mod
 * @param {Uint8Array} bytes
 * @param {(ptr: number) => T} fn
 * @returns {T}
 */
function withInput(mod, bytes, fn) {
  const ptr = mod._malloc(bytes.byteLength);
  if (!ptr) throw new TexCodecError("malloc", TexResult.OUT_OF_MEMORY,
                                    "out of memory");
  try {
    mod.HEAPU8.set(bytes, ptr);
    return fn(ptr);
  } finally {
    mod._free(ptr);
  }
}

/**
 * @param {TexCodecModule} mod
 * @param {string} operation
 * @param {number} code
 * @returns {never}
 */
function throwResult(mod, operation, code) {
  const strPtr = mod._texc_result_str(code);
  throw new TexCodecError(operation, code,
                          strPtr ? mod.UTF8ToString(strPtr) : undefined);
}

/** Reverse lookup for error messages / debugging. @param {number} fmt */
function formatName(fmt) {
  for (const [k, v] of Object.entries(TexFormat)) if (v === fmt) return k;
  return `format#${fmt}`;
}

/* ----------------------------------------------------------------- codec */

/**
 * Decoding: compressed texture data -> RGBA pixels.
 * Obtain via {@link TexCodec#decoder}; all methods are async and
 * self-initialising.
 */
export class TexDecoder {
  /** @param {TexCodec} owner @package */
  constructor(owner) { /** @private */ this._owner = owner; }

  /**
   * Decode compressed data to 8-bit RGBA (returns `width*height*4` bytes).
   * @param {number} format a {@link TexFormat} value
   * @param {Uint8Array} data compressed data, at least
   *        `encodedSize(format, width, height)` bytes
   * @param {number} width final image width in pixels
   * @param {number} height final image height in pixels (for the
   *        `*_A_ATLAS` formats this is the half-height FINAL size; the
   *        double-height atlas layout is handled internally)
   * @returns {Promise<Uint8Array>} tightly packed RGBA, top-left origin
   */
  async decode(format, data, width, height) {
    const mod = await this._owner._mod();
    return withInput(mod, data, (src) => {
      const out = mod._texc_decode_alloc(format, src, data.byteLength,
                                         width, height);
      if (!out) throwResult(mod, `decode(${formatName(format)})`,
                            mod._texc_last_error());
      const rgba = mod.HEAPU8.slice(out, out + width * height * 4);
      mod._texc_free(out);
      return rgba;
    });
  }

  /**
   * Decode to 32-bit float RGBA - full range for HDR sources (BC6H, ASTC
   * HDR blocks); LDR formats come back normalised to [0, 1].
   * @param {number} format a {@link TexFormat} value
   * @param {Uint8Array} data compressed data
   * @param {number} width
   * @param {number} height
   * @returns {Promise<Float32Array>} `width*height*4` floats
   */
  async decodeF32(format, data, width, height) {
    const mod = await this._owner._mod();
    return withInput(mod, data, (src) => {
      const out = mod._texc_decode_f32_alloc(format, src, data.byteLength,
                                             width, height);
      if (!out) throwResult(mod, `decodeF32(${formatName(format)})`,
                            mod._texc_last_error());
      const px = new Float32Array(
          mod.HEAPF32.buffer.slice(out, out + width * height * 16));
      mod._texc_free(out);
      return px;
    });
  }

  /**
   * Unswizzle platform-tiled compressed data and decode it in one call.
   * @param {number} mode a {@link SwizzleMode} value
   * @param {number} format a {@link TexFormat} value
   * @param {Uint8Array} data tiled compressed data
   *        (`swizzledSize(mode, format, w, h, arg)` bytes)
   * @param {number} width
   * @param {number} height
   * @param {number} [arg=0] mode-specific parameter (see {@link SwizzleMode})
   * @returns {Promise<Uint8Array>} RGBA8 pixels
   */
  async decodeSwizzled(mode, format, data, width, height, arg = 0) {
    const mod = await this._owner._mod();
    return withInput(mod, data, (src) => {
      const out = mod._texc_decode_swizzled_alloc(mode, format, src,
                                                  data.byteLength,
                                                  width, height, arg);
      if (!out) throwResult(mod, `decodeSwizzled(${formatName(format)})`,
                            mod._texc_last_error());
      const rgba = mod.HEAPU8.slice(out, out + width * height * 4);
      mod._texc_free(out);
      return rgba;
    });
  }
}

/**
 * Encoding: RGBA pixels -> compressed texture data.
 * Obtain via {@link TexCodec#encoder}.
 */
export class TexEncoder {
  /** @param {TexCodec} owner @package */
  constructor(owner) { /** @private */ this._owner = owner; }

  /**
   * Compressed size in bytes of a `width x height` image.
   * @param {number} format a {@link TexFormat} value
   * @param {number} width
   * @param {number} height
   * @returns {Promise<number>}
   */
  async encodedSize(format, width, height) {
    const mod = await this._owner._mod();
    return mod._texc_encoded_size(format, width, height);
  }

  /**
   * Encode 8-bit RGBA pixels.
   * @param {number} format a {@link TexFormat} value
   * @param {Uint8Array} rgba `width*height*4` bytes, tightly packed
   * @param {number} width
   * @param {number} height
   * @param {{alphaThreshold?: number}} [options] encoder options:
   *        `alphaThreshold` (0-256, default 128) - pixels with alpha below
   *        it become transparent in punchthrough formats (BC1 3-colour
   *        mode, ETC2_RGBA1).
   * @returns {Promise<Uint8Array>} compressed data
   *          (`encodedSize(format, width, height)` bytes)
   */
  async encode(format, rgba, width, height, options = {}) {
    const mod = await this._owner._mod();
    const threshold = options.alphaThreshold ?? 128;
    return withInput(mod, rgba, (src) => {
      const encSize = mod._texc_encoded_size(format, width, height);
      if (!encSize) throwResult(mod, `encode(${formatName(format)})`,
                                TexResult.INVALID_ARG);
      const dst = mod._malloc(encSize);
      if (!dst) throwResult(mod, "malloc", TexResult.OUT_OF_MEMORY);
      try {
        const rc = mod._texc_encode_with_options(
            format, src, rgba.byteLength, width, height, dst, encSize,
            threshold);
        if (rc !== TexResult.OK)
          throwResult(mod, `encode(${formatName(format)})`, rc);
        return mod.HEAPU8.slice(dst, dst + encSize);
      } finally {
        mod._free(dst);
      }
    });
  }
}

/**
 * Platform tiling: convert compressed (or raw RGBA8) data between
 * console-tiled and linear row-major block order.
 * Obtain via {@link TexCodec#swizzler}.
 */
export class TexSwizzler {
  /** @param {TexCodec} owner @package */
  constructor(owner) { /** @private */ this._owner = owner; }

  /**
   * Size in bytes of the tiled representation (>= the linear size because
   * of tile padding).
   * @param {number} mode a {@link SwizzleMode} value
   * @param {number} format a {@link TexFormat} value
   * @param {number} width
   * @param {number} height
   * @param {number} [arg=0]
   * @returns {Promise<number>}
   */
  async swizzledSize(mode, format, width, height, arg = 0) {
    const mod = await this._owner._mod();
    return mod._texc_swizzled_size(mode, format, width, height, arg);
  }

  /**
   * Size in bytes of the LINEAR side - what {@link TexSwizzler#unswizzle}
   * returns and {@link TexSwizzler#reswizzle} consumes. Normally the
   * format's encoded size, but PS Vita raw honours the bytes-per-pixel
   * `arg`, so prefer this whenever you pass a non-zero `arg`.
   * @param {number} mode a {@link SwizzleMode} value
   * @param {number} format a {@link TexFormat} value
   * @param {number} width
   * @param {number} height
   * @param {number} [arg=0]
   * @returns {Promise<number>}
   */
  async unswizzledSize(mode, format, width, height, arg = 0) {
    const mod = await this._owner._mod();
    return mod._texc_unswizzled_size(mode, format, width, height, arg);
  }

  /**
   * UNSWIZZLE: platform-tiled -> linear row-major block order - the
   * direction you need BEFORE decoding console data.
   * @param {number} mode a {@link SwizzleMode} value
   * @param {number} format a {@link TexFormat} value
   * @param {Uint8Array} data tiled data (`swizzledSize(...)` bytes)
   * @param {number} width image width in PIXELS
   * @param {number} height image height in PIXELS
   * @param {number} [arg=0] mode-specific (see {@link SwizzleMode})
   * @returns {Promise<Uint8Array>} linear compressed data
   */
  async unswizzle(mode, format, data, width, height, arg = 0) {
    return this._convert(mode, format, data, width, height, arg, 1);
  }

  /**
   * RESWIZZLE: linear row-major -> platform-tiled block order - the exact
   * inverse of {@link TexSwizzler#unswizzle}; use it to put the platform
   * tiling BACK when repacking data for the target hardware (e.g. into a
   * G1T).
   * @param {number} mode a {@link SwizzleMode} value
   * @param {number} format a {@link TexFormat} value
   * @param {Uint8Array} data linear compressed data
   * @param {number} width
   * @param {number} height
   * @param {number} [arg=0]
   * @returns {Promise<Uint8Array>} tiled data (may be larger than the input
   *          because of tile padding)
   */
  async reswizzle(mode, format, data, width, height, arg = 0) {
    return this._convert(mode, format, data, width, height, arg, 0);
  }

  /**
   * Legacy alias of {@link TexSwizzler#reswizzle} (linear -> tiled).
   * Prefer `reswizzle` - the name states the direction unambiguously.
   * @deprecated use reswizzle
   * @param {number} mode @param {number} format @param {Uint8Array} data
   * @param {number} width @param {number} height @param {number} [arg=0]
   * @returns {Promise<Uint8Array>}
   */
  async swizzle(mode, format, data, width, height, arg = 0) {
    return this.reswizzle(mode, format, data, width, height, arg);
  }

  /** @private */
  async _convert(mode, format, data, width, height, arg, toLinear) {
    const mod = await this._owner._mod();
    return withInput(mod, data, (src) => {
      const sizePtr = mod._malloc(4);
      try {
        const out = mod._texc_swizzle_alloc(mode, format, src,
                                            data.byteLength, width, height,
                                            arg, toLinear, sizePtr);
        if (!out) throwResult(mod,
                              toLinear ? "unswizzle" : "reswizzle",
                              mod._texc_last_error());
        const size = mod.getValue(sizePtr, "i32") >>> 0;
        const bytes = mod.HEAPU8.slice(out, out + size);
        mod._texc_free(out);
        return bytes;
      } finally {
        mod._free(sizePtr);
      }
    });
  }
}

/**
 * Raw-pixel utilities: colour profile conversion, flipping and cropping
 * (ported from tex-decoder's profiler.ts / flipper.ts).
 * Obtain via {@link TexCodec#image}.
 */
export class TexImage {
  /** @param {TexCodec} owner @package */
  constructor(owner) { /** @private */ this._owner = owner; }

  /**
   * Bytes per pixel of a profile (e.g. RGBA8 -> 4, RGB565 -> 2).
   * @param {number} profile a {@link PixelProfile} value
   * @returns {Promise<number>}
   */
  async profileBytesPerPixel(profile) {
    const mod = await this._owner._mod();
    return mod._texc_profile_bytes_per_pixel(profile);
  }

  /**
   * Convert raw pixels between colour profiles (tex-decoder
   * `convertProfile` semantics: proportional range scaling, missing source
   * alpha becomes opaque, missing colour channels become 0, float profiles
   * treat 1.0 as the integer maximum).
   * @param {number} srcProfile a {@link PixelProfile} value
   * @param {number} dstProfile a {@link PixelProfile} value
   * @param {Uint8Array} data source pixels
   * @param {number} [pixelCount] pixels to convert; default derives from
   *        `data.length / bytesPerPixel(srcProfile)`
   * @returns {Promise<Uint8Array>} converted pixels
   *          (`pixelCount * bytesPerPixel(dstProfile)` bytes)
   */
  async convertProfile(srcProfile, dstProfile, data, pixelCount) {
    const mod = await this._owner._mod();
    const sbpp = mod._texc_profile_bytes_per_pixel(srcProfile);
    const dbpp = mod._texc_profile_bytes_per_pixel(dstProfile);
    if (!sbpp || !dbpp)
      throwResult(mod, "convertProfile", TexResult.INVALID_ARG);
    const count = pixelCount ?? Math.floor(data.byteLength / sbpp);
    return withInput(mod, data, (src) => {
      const outSize = count * dbpp;
      const dst = mod._malloc(outSize);
      if (!dst) throwResult(mod, "malloc", TexResult.OUT_OF_MEMORY);
      try {
        const rc = mod._texc_convert_profile(srcProfile, src, data.byteLength,
                                             dstProfile, dst, outSize, count);
        if (rc !== TexResult.OK) throwResult(mod, "convertProfile", rc);
        return mod.HEAPU8.slice(dst, dst + outSize);
      } finally {
        mod._free(dst);
      }
    });
  }

  /**
   * Flip raw pixel rows vertically (tex-decoder `flipImage`).
   * @param {Uint8Array} data raw pixels
   * @param {number} width @param {number} height
   * @param {number} [bytesPerPixel=4]
   * @returns {Promise<Uint8Array>} flipped copy
   */
  async flipY(data, width, height, bytesPerPixel = 4) {
    return this._flip("_texc_flip_y", data, width, height, bytesPerPixel);
  }

  /**
   * Mirror raw pixels horizontally.
   * @param {Uint8Array} data raw pixels
   * @param {number} width @param {number} height
   * @param {number} [bytesPerPixel=4]
   * @returns {Promise<Uint8Array>} mirrored copy
   */
  async flipX(data, width, height, bytesPerPixel = 4) {
    return this._flip("_texc_flip_x", data, width, height, bytesPerPixel);
  }

  /** @private */
  async _flip(fn, data, width, height, bytesPerPixel) {
    const mod = await this._owner._mod();
    return withInput(mod, data, (src) => {
      const size = width * height * bytesPerPixel;
      const dst = mod._malloc(size);
      if (!dst) throwResult(mod, "malloc", TexResult.OUT_OF_MEMORY);
      try {
        const rc = mod[fn](src, data.byteLength, dst, size,
                           width, height, bytesPerPixel);
        if (rc !== TexResult.OK) throwResult(mod, fn.slice(6), rc);
        return mod.HEAPU8.slice(dst, dst + size);
      } finally {
        mod._free(dst);
      }
    });
  }

  /**
   * Copy a rectangle out of a raw image (tex-decoder `cropImage`).
   * @param {Uint8Array} data raw pixels
   * @param {number} width source width @param {number} height source height
   * @param {number} bytesPerPixel e.g. 4 for RGBA8
   * @param {number} x crop origin @param {number} y crop origin
   * @param {number} cropWidth @param {number} cropHeight
   * @returns {Promise<Uint8Array>} `cropWidth * cropHeight * bytesPerPixel`
   *          bytes
   */
  async crop(data, width, height, bytesPerPixel, x, y, cropWidth,
             cropHeight) {
    const mod = await this._owner._mod();
    return withInput(mod, data, (src) => {
      const size = cropWidth * cropHeight * bytesPerPixel;
      const dst = mod._malloc(size);
      if (!dst) throwResult(mod, "malloc", TexResult.OUT_OF_MEMORY);
      try {
        const rc = mod._texc_crop(src, data.byteLength, width, height,
                                  bytesPerPixel, x, y, cropWidth, cropHeight,
                                  dst, size);
        if (rc !== TexResult.OK) throwResult(mod, "crop", rc);
        return mod.HEAPU8.slice(dst, dst + size);
      } finally {
        mod._free(dst);
      }
    });
  }
}

/* ------------------------------------------------------------- main class */

/**
 * Entry point. Wraps one initialised WASM module and exposes the three
 * functional areas as {@link TexDecoder}, {@link TexEncoder} and
 * {@link TexSwizzler}.
 */
export class TexCodec {
  /**
   * @param {TexCodecModule} module initialised Emscripten module
   * @package use {@link TexCodec.load} instead
   */
  constructor(module) {
    /** Raw Emscripten module - every C export is reachable here
     *  (`module._texc_decode_bc7(...)` etc.). @type {TexCodecModule} */
    this.module = module;
    /** @type {TexDecoder} */  this.decoder  = new TexDecoder(this);
    /** @type {TexEncoder} */  this.encoder  = new TexEncoder(this);
    /** @type {TexSwizzler} */ this.swizzler = new TexSwizzler(this);
    /** @type {TexImage} */    this.image    = new TexImage(this);
  }

  /**
   * Load (or reuse) the WASM module and return a ready instance. Safe to
   * call repeatedly - the default module is created once and shared.
   * @param {{factory?: () => Promise<TexCodecModule>,
   *          moduleUrl?: string | URL}} [options]
   *        `factory`: the function exported by tex_codec.js, if you import
   *        it yourself (bundlers). `moduleUrl`: URL of tex_codec.js to
   *        dynamically import (default: "./tex_codec.js" next to this file).
   * @returns {Promise<TexCodec>}
   */
  static async load(options = {}) {
    if (options.factory) return new TexCodec(await options.factory());
    if (options.moduleUrl) {
      const factory = (await import(/* webpackIgnore: true */
                                    options.moduleUrl.toString())).default;
      return new TexCodec(await factory());
    }
    defaultModulePromise ??= import(/* webpackIgnore: true */
                                    new URL("./tex_codec.js",
                                            import.meta.url).href)
        .then((m) => m.default());
    return new TexCodec(await defaultModulePromise);
  }

  /**
   * Version of the loaded WASM binary.
   * @returns {Promise<{major: number, minor: number, patch: number,
   *                    string: string, wrapper: string}>}
   *          `string` is the binary's version ("1.1.0"), `wrapper` is this
   *          JS file's {@link VERSION}.
   */
  async version() {
    const mod = await this._mod();
    const v = mod._texc_version();
    return {
      major: v >>> 16,
      minor: (v >>> 8) & 0xff,
      patch: v & 0xff,
      string: mod.UTF8ToString(mod._texc_version_string()),
      wrapper: VERSION,
    };
  }

  /**
   * One-line build identification of the WASM binary, e.g.
   * `"tex_codec 1.1.0 (git 3f2a1b8, built Aug 19 2026, Emscripten, wasm32,
   * wasm)"` - paste this into bug reports.
   * @returns {Promise<string>}
   */
  async buildInfo() {
    const mod = await this._mod();
    return mod.UTF8ToString(mod._texc_build_info());
  }

  /**
   * Verify the loaded WASM binary matches this wrapper's {@link VERSION}.
   * Catches a stale `tex_codec.wasm` shipped beside a newer
   * `tex_codec_api.mjs` (or vice versa).
   * @param {{throwOnMismatch?: boolean}} [options] throw instead of
   *        returning false (default false)
   * @returns {Promise<boolean>} true when the versions agree
   */
  async checkVersion(options = {}) {
    const { string: binary } = await this.version();
    const ok = binary === VERSION;
    if (!ok && options.throwOnMismatch)
      throw new TexCodecError(
          "version check", TexResult.UNSUPPORTED,
          `wrapper ${VERSION} but tex_codec.wasm reports ${binary}`);
    return ok;
  }

  /**
   * Human-readable name of a format ("BC7", "ASTC_6x6", ...).
   * @param {number} format @returns {Promise<string | null>}
   */
  async formatName(format) {
    const mod = await this._mod();
    const p = mod._texc_format_name(format);
    return p ? mod.UTF8ToString(p) : null;
  }

  /**
   * Block geometry of a format.
   * @param {number} format a {@link TexFormat} value
   * @returns {Promise<{width: number, height: number, bytes: number}>}
   */
  async blockDims(format) {
    const mod = await this._mod();
    const buf = mod._malloc(12);
    try {
      const rc = mod._texc_block_dims(format, buf, buf + 4, buf + 8);
      if (rc !== TexResult.OK) throwResult(mod, "blockDims", rc);
      return {
        width:  mod.getValue(buf, "i32"),
        height: mod.getValue(buf + 4, "i32"),
        bytes:  mod.getValue(buf + 8, "i32"),
      };
    } finally {
      mod._free(buf);
    }
  }

  /** @package @returns {Promise<TexCodecModule>} */
  async _mod() { return this.module; }
}

/* --------------------------------------------- per-format/mode helpers */
/*
 * Generated convenience methods, so call sites don't need the enums:
 *
 *   tex.decoder.decodeBC1(data, w, h)            === decode(TexFormat.BC1, ...)
 *   tex.decoder.decodeF32BC6H_UF16(data, w, h)   === decodeF32(...)
 *   tex.encoder.encodeETC2_RGBA8(rgba, w, h, o)  === encode(...)
 *   tex.encoder.encodedSizeASTC_4x4(w, h)        === encodedSize(...)
 *   tex.swizzler.unswizzlePS4(fmt, data, w, h, arg)   tiled -> linear
 *   tex.swizzler.reswizzlePS4(fmt, data, w, h, arg)   linear -> tiled
 *   tex.swizzler.swizzledSizePS4(fmt, w, h, arg)
 *
 * One method per TexFormat key (minus INVALID) and per SwizzleMode key
 * (minus NONE); mode names are prettified per k_modeNames below. Typed
 * individually in tex_codec_api.d.ts.
 */

for (const [name, fmt] of Object.entries(TexFormat)) {
  if (fmt === TexFormat.INVALID) continue;
  TexDecoder.prototype[`decode${name}`] = function (data, width, height) {
    return this.decode(fmt, data, width, height);
  };
  TexDecoder.prototype[`decodeF32${name}`] = function (data, width, height) {
    return this.decodeF32(fmt, data, width, height);
  };
  TexEncoder.prototype[`encode${name}`] =
      function (rgba, width, height, options) {
    return this.encode(fmt, rgba, width, height, options);
  };
  TexEncoder.prototype[`encodedSize${name}`] = function (width, height) {
    return this.encodedSize(fmt, width, height);
  };
}

const k_modeNames = Object.freeze({
  PS4: "PS4", PS5: "PS5", SWITCH: "Switch", PSVITA: "PSVita", X360: "X360",
  PSP: "PSP", N3DS: "N3DS", WIIU: "WiiU", DX12_64KB: "DX12_64KB",
});

for (const [key, pretty] of Object.entries(k_modeNames)) {
  const mode = SwizzleMode[key];
  TexSwizzler.prototype[`unswizzle${pretty}`] =
      function (format, data, width, height, arg = 0) {
    return this.unswizzle(mode, format, data, width, height, arg);
  };
  TexSwizzler.prototype[`reswizzle${pretty}`] =
      function (format, data, width, height, arg = 0) {
    return this.reswizzle(mode, format, data, width, height, arg);
  };
  TexSwizzler.prototype[`swizzledSize${pretty}`] =
      function (format, width, height, arg = 0) {
    return this.swizzledSize(mode, format, width, height, arg);
  };
  TexDecoder.prototype[`decodeSwizzled${pretty}`] =
      function (format, data, width, height, arg = 0) {
    return this.decodeSwizzled(mode, format, data, width, height, arg);
  };
}
