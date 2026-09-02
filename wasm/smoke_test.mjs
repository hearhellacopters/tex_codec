/* smoke_test.mjs - Node sanity check for the WASM build, exercising the
 * ergonomic wrapper (tex_codec_api.mjs), the named per-format/per-mode
 * exports, encoder options, and the G1T alpha-atlas formats.
 * Usage: node wasm/smoke_test.mjs [dist_dir]   (default ../dist)         */
import { fileURLToPath, pathToFileURL } from "url";
import { dirname, join, resolve } from "path";

const here = dirname(fileURLToPath(import.meta.url));
const dist = resolve(process.argv[2] ?? join(here, "..", "dist"));
const api = await import(pathToFileURL(join(dist, "tex_codec_api.mjs")).href);
const { TexCodec, TexFormat, SwizzleMode, SwitchBlockHeightAuto,
        PixelProfile, TexCodecError, VERSION } = api;

let failures = 0;
const check = (cond, msg) => {
  if (!cond) { failures++; console.error("FAIL:", msg); }
  else console.log("ok:", msg);
};

const tex = await TexCodec.load({
  moduleUrl: pathToFileURL(join(dist, "tex_codec.js")),
});
const mod = tex.module;

/* ---- version tracking ---- */
const ver = await tex.version();
check(ver.string === VERSION,
      `wasm binary ${ver.string} matches wrapper VERSION ${VERSION}`);
check(ver.major >= 1 && `${ver.major}.${ver.minor}.${ver.patch}` === ver.string,
      `version fields agree with string (${ver.string})`);
check(await tex.checkVersion() === true, "checkVersion() passes");
const build = await tex.buildInfo();
check(build.includes(VERSION) && build.includes("git"),
      `buildInfo: ${build}`);

/* ---- wrapper basics ---- */
check(await tex.formatName(TexFormat.BC7) === "BC7", "formatName(BC7)");
const bd = await tex.blockDims(TexFormat.ASTC_8x8);
check(bd.width === 8 && bd.height === 8 && bd.bytes === 16,
      "blockDims(ASTC_8x8)");

/* ---- gradient helper ---- */
function gradient(w, h, alphaRamp = false) {
  const img = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      img[i] = Math.round(x * 255 / (w - 1));
      img[i + 1] = Math.round(y * 255 / (h - 1));
      img[i + 2] = 128;
      img[i + 3] = alphaRamp ? 32 + Math.round(y * 192 / (h - 1)) : 255;
    }
  return img;
}
const psnr = (a, b, stride = 1) => {
  let sse = 0, n = 0;
  for (let i = 0; i < a.length; i += stride) { sse += (a[i] - b[i]) ** 2; n++; }
  return 10 * Math.log10(255 * 255 / (sse / n));
};

/* ---- decoder/encoder classes, several formats ---- */
for (const name of ["BC7", "ETC2_RGBA8", "ASTC_4x4"]) {
  const fmt = TexFormat[name];
  const w = 32, h = 32;
  const img = gradient(w, h);
  const enc = await tex.encoder.encode(fmt, img, w, h);
  check(enc.length === await tex.encoder.encodedSize(fmt, w, h),
        `${name} encode size`);
  const dec = await tex.decoder.decode(fmt, enc, w, h);
  const p = psnr(img, dec);
  check(p > 28, `${name} class roundtrip PSNR ${p.toFixed(1)} dB > 28`);
}

/* ---- decodeF32 agrees with decode ---- */
{
  const w = 16, h = 16;
  const img = gradient(w, h);
  const enc = await tex.encoder.encode(TexFormat.BC4, img, w, h);
  const dec8 = await tex.decoder.decode(TexFormat.BC4, enc, w, h);
  const decF = await tex.decoder.decodeF32(TexFormat.BC4, enc, w, h);
  let bad = 0;
  for (let i = 0; i < decF.length; i++)
    if (Math.abs(Math.round(decF[i] * 255) - dec8[i]) > 2) bad++;
  check(bad === 0, "decodeF32 agrees with decode (BC4)");
}

/* ---- alpha atlas: encode -> decode folds alpha ---- */
{
  const w = 32, h = 32;
  const img = gradient(w, h, true);
  const enc = await tex.encoder.encode(TexFormat.ETC1_RGB_A_ATLAS, img, w, h);
  const baseSize = await tex.encoder.encodedSize(TexFormat.ETC1_RGB, w, h);
  check(enc.length === 2 * baseSize, "ETC1 atlas size == 2x base");
  const dec = await tex.decoder.decode(TexFormat.ETC1_RGB_A_ATLAS, enc, w, h);
  let worstA = 0;
  for (let i = 3; i < img.length; i += 4)
    worstA = Math.max(worstA, Math.abs(img[i] - dec[i]));
  check(worstA <= 8, `ETC1 atlas folded alpha err ${worstA} <= 8`);
  const pRGB = Math.min(psnr(img, dec, 4),
                        psnr(img.subarray(1), dec.subarray(1), 4));
  check(pRGB > 26, `ETC1 atlas RGB PSNR ${pRGB.toFixed(1)} dB > 26`);
}

/* ---- encoder options: alphaThreshold flips BC1 punchthrough ---- */
{
  const w = 4, h = 4;
  const img = new Uint8Array(w * h * 4);
  for (let i = 0; i < img.length; i += 4) {
    img[i] = 200; img[i + 1] = 100; img[i + 2] = 50; img[i + 3] = 100;
  }
  const opaque = await tex.decoder.decode(TexFormat.BC1,
      await tex.encoder.encode(TexFormat.BC1, img, w, h,
                               { alphaThreshold: 50 }), w, h);
  const transp = await tex.decoder.decode(TexFormat.BC1,
      await tex.encoder.encode(TexFormat.BC1, img, w, h,
                               { alphaThreshold: 200 }), w, h);
  check(opaque[3] === 255 && transp[3] === 0,
        `alphaThreshold: 50 -> A=${opaque[3]}, 200 -> A=${transp[3]}`);
}

/* ---- swizzler class + atlas swizzling ---- */
{
  const w = 64, h = 64;
  const linSize = mod._texc_encoded_size(TexFormat.BC7, w, h);
  const lin = new Uint8Array(linSize).map((_, i) => (i * 7 + 13) & 0xff);
  const tiled = await tex.swizzler.swizzle(SwizzleMode.SWITCH, TexFormat.BC7,
                                           lin, w, h, SwitchBlockHeightAuto);
  check(tiled.length >= linSize, "SWITCH swizzled size >= linear");
  const back = await tex.swizzler.unswizzle(SwizzleMode.SWITCH, TexFormat.BC7,
                                            tiled, w, h,
                                            SwitchBlockHeightAuto);
  check(back.length === linSize && back.every((b, i) => b === lin[i]),
        "SWITCH swizzle->unswizzle identity (class API)");

  const atlasSize = await tex.swizzler.swizzledSize(
      SwizzleMode.PS4, TexFormat.ETC1_RGB_A_ATLAS, w, h);
  const atlasBase = await tex.swizzler.swizzledSize(
      SwizzleMode.PS4, TexFormat.ETC1_RGB, w, 2 * h);
  check(atlasSize === atlasBase && atlasSize > 0,
        "atlas swizzled size == base at double height");
}

/* ---- error surface ---- */
try {
  await tex.decoder.decode(TexFormat.BC7, new Uint8Array(4), 64, 64);
  check(false, "short-buffer decode should throw");
} catch (e) {
  check(e instanceof TexCodecError && e.code === -2,
        `short-buffer decode throws TexCodecError code ${e.code}`);
}

/* ---- named per-format / per-mode exports (raw pointer layer) ---- */
{
  check(typeof mod._texc_decode_bc7 === "function" &&
        typeof mod._texc_encode_etc2_rgba8 === "function" &&
        typeof mod._texc_encoded_size_astc_12x12 === "function" &&
        typeof mod._texc_unswizzle_ps4 === "function" &&
        typeof mod._texc_swizzled_size_switch === "function" &&
        typeof mod._texc_encode_etc1_rgb_a_atlas === "function",
        "named exports present");

  // decode a solid-red BC1 block through the named export
  const bc1 = new Uint8Array([0x00, 0xf8, 0x00, 0xf8, 0, 0, 0, 0]);
  const src = mod._malloc(8), dst = mod._malloc(64);
  mod.HEAPU8.set(bc1, src);
  const rc = mod._texc_decode_bc1(src, 8, 4, 4, dst, 64);
  const px = mod.HEAPU8.subarray(dst, dst + 4);
  check(rc === 0 && px[0] === 255 && px[1] === 0 && px[2] === 0,
        "_texc_decode_bc1 solid red");
  mod._free(src); mod._free(dst);
}

/* ---- per-format / per-mode helper methods on the classes ---- */
{
  const w = 16, h = 16;
  const img = gradient(w, h);
  const enc = await tex.encoder.encodeBC1(img, w, h);
  check(enc.length === await tex.encoder.encodedSizeBC1(w, h),
        "encodeBC1/encodedSizeBC1 helpers");
  const dec = await tex.decoder.decodeBC1(enc, w, h);
  check(psnr(img, dec) > 26, "decodeBC1 helper roundtrip");

  const lin = new Uint8Array(await tex.encoder.encodedSizeBC7(64, 64))
      .map((_, i) => (i * 11 + 3) & 0xff);
  const tiled = await tex.swizzler.reswizzlePS4(TexFormat.BC7, lin, 64, 64);
  const back = await tex.swizzler.unswizzlePS4(TexFormat.BC7, tiled, 64, 64);
  check(back.every((b, i) => b === lin[i]),
        "reswizzlePS4 -> unswizzlePS4 identity (helpers)");
  check(typeof tex.swizzler.reswizzleSwitch === "function" &&
        typeof tex.swizzler.unswizzleWiiU === "function" &&
        typeof tex.decoder.decodeSwizzledPSVita === "function" &&
        typeof tex.decoder.decodeETC1_RGB_A_ATLAS === "function" &&
        typeof tex.encoder.encodeASTC_12x12 === "function",
        "helper method coverage");
  // deprecated alias still works and matches reswizzle
  const tiled2 = await tex.swizzler.swizzle(SwizzleMode.PS4, TexFormat.BC7,
                                            lin, 64, 64);
  check(tiled2.every((b, i) => b === tiled[i]),
        "legacy swizzle() === reswizzle()");
}

/* ---- image utilities (tex.image) ---- */
{
  // profile conversion: RGBA8 -> BGRA8 exact swap
  const px = new Uint8Array([10, 20, 30, 40, 200, 150, 100, 50]);
  const bgra = await tex.image.convertProfile(PixelProfile.RGBA8,
                                              PixelProfile.BGRA8, px);
  check(bgra[0] === 30 && bgra[1] === 20 && bgra[2] === 10 && bgra[3] === 40,
        "convertProfile RGBA8 -> BGRA8");
  check(await tex.image.profileBytesPerPixel(PixelProfile.RGB565) === 2,
        "profileBytesPerPixel(RGB565)");

  // 565 roundtrip within quantisation
  const p565 = await tex.image.convertProfile(PixelProfile.RGBA8,
                                              PixelProfile.RGB565, px, 1);
  const back = await tex.image.convertProfile(PixelProfile.RGB565,
                                              PixelProfile.RGBA8, p565, 1);
  check(Math.abs(back[0] - 10) <= 8 && Math.abs(back[1] - 20) <= 4 &&
        Math.abs(back[2] - 30) <= 8 && back[3] === 255,
        "RGB565 roundtrip within quantisation");

  // flipY / flipX / crop
  const img = new Uint8Array(2 * 2 * 4).map((_, i) => i);
  const fy = await tex.image.flipY(img, 2, 2, 4);
  check(fy[0] === 8 && fy[8] === 0, "flipY swaps rows");
  const fy2 = await tex.image.flipY(fy, 2, 2, 4);
  check(fy2.every((b, i) => b === img[i]), "flipY twice = identity");
  const fx = await tex.image.flipX(img, 2, 2, 4);
  check(fx[0] === 4 && fx[4] === 0, "flipX mirrors pixels");
  const big = new Uint8Array(4 * 4).map((_, i) => i);
  const crop = await tex.image.crop(big, 4, 4, 1, 1, 1, 2, 2);
  check(crop[0] === 5 && crop[1] === 6 && crop[2] === 9 && crop[3] === 10,
        "crop KAT");
}

/* ---- PICA200 (3DS) ETC1 ---- */
{
  const w = 64, h = 64;
  const img = gradient(w, h);
  for (const name of ["PICA_ETC1_RGB8", "PICA_ETC1_RGB8A4"]) {
    const fmt = TexFormat[name];
    const bpp = name.endsWith("A4") ? 1 : 0.5;
    const enc = await tex.encoder.encode(fmt, img, w, h);
    check(enc.length === w * h * bpp, `${name} size ${enc.length} (${bpp}B/px)`);
    const dec = await tex.decoder.decode(fmt, enc, w, h);
    const p = psnr(img, dec);
    check(p > 26, `${name} roundtrip PSNR ${p.toFixed(1)} dB > 26`);
  }
  const bd = await tex.blockDims(TexFormat.PICA_ETC1_RGB8);
  check(bd.width === 8 && bd.height === 8 && bd.bytes === 32,
        "PICA_ETC1_RGB8 block dims are the 8x8 tile");
  check(typeof tex.decoder.decodePICA_ETC1_RGB8 === "function" &&
        typeof tex.encoder.encodePICA_ETC1_RGB8A4 === "function" &&
        typeof mod._texc_decode_pica_etc1_rgb8 === "function",
        "PICA helpers + named exports present");
}

/* ---- reswizzle raw exports ---- */
check(typeof mod._texc_reswizzle === "function" &&
      typeof mod._texc_reswizzle_ps4 === "function" &&
      typeof mod._texc_reswizzle_switch === "function" &&
      typeof mod._texc_convert_profile === "function" &&
      typeof mod._texc_flip_y === "function" &&
      typeof mod._texc_crop === "function",
      "reswizzle + image-util raw exports present");

console.log(failures ? `\n${failures} FAILURE(S)` : "\nWASM SMOKE TEST PASSED");
process.exitCode = failures ? 1 : 0;
