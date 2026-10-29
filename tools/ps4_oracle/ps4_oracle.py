"""PS4 tiling oracle.

Runs Sony's GpuAddress code, statically linked into a KTGL PS4 eboot
(Stranger of Paradise, eboot.bin), natively through ps4_host.dll. Nothing
from the eboot is copied; it is executed as a black box, the same way
tools/ps5_oracle checks against the PS5 SDK DLL.

Entry points (eboot-SOP.bin, image base 0):
  0x22B4280  KTGL 2D/array texture setup: G1T format index -> tile mode,
             per-mip size and pitch (the same call the G1T loader makes,
             which is also the on-disk layout when the G1T system is 0x0B)
  0x22B8010  KTGL volume (G1T type 2) setup: (w, h, depth, mips, format,
             0, out) into the same 888-byte block
  0x1DA70    GpuAddress tileSurface(dst, src, const TilingParameters*)

Usage:
  python tools/ps4_oracle/ps4_oracle.py layout <w> <h> <mips> <g1t_fmt> [layers]
  python tools/ps4_oracle/ps4_oracle.py verify
  python tools/ps4_oracle/ps4_oracle.py kat > tests/ps4_volume_kat.h
Environment:
  TEXC_PS4_EBOOT  path to the decrypted eboot (ELF)
  TEXC_PS4_HOST   path to ps4_host.dll (default: build/ps4_host.dll)
  TEXC_CLI        path to texc.exe for the codec comparison in verify
                  (default: build/Release/texc.exe)
"""
import ctypes
import os
import random
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EBOOT = os.environ.get("TEXC_PS4_EBOOT", r"C:\Users\SalP\Documents\Reversing\_KTGL\11_PS4\eboot-SOP.bin")
HOST_DLL = os.environ.get("TEXC_PS4_HOST", os.path.join(REPO, "build", "ps4_host.dll"))
TEXC = os.environ.get("TEXC_CLI", os.path.join(REPO, "build", "Release", "texc.exe"))

FN_KT_LAYOUT = 0x22B4280
FN_KT_VOLUME = 0x22B8010
FN_TILE = 0x1DA70
FORMAT_TABLE = 0x2FC9E80
STUB_MEMCPY = 0x2C6FA70
STUB_MEMSET = 0x2C71720
STUB_STACK_CHK_FAIL = 0x2C6FA60
GOT_STACK_CHK_GUARD = 0x36177C8

# GpuAddress tile modes KTGL picks for textures
TILE_MODE_NAMES = {5: "Depth_1dThin", 8: "Display_LinearAligned", 13: "Thin_1dThin",
                   19: "Thick_1dThick"}


class Oracle:
    def __init__(self, eboot=EBOOT, host=HOST_DLL):
        self.dll = dll = ctypes.CDLL(host)
        dll.ps4o_init.argtypes = [ctypes.c_wchar_p] + [ctypes.c_uint64] * 4
        dll.ps4o_base.restype = ctypes.c_uint64
        dll.ps4o_call.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint32,
                                  ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
        dll.ps4o_call.restype = ctypes.c_uint32
        rc = dll.ps4o_init(eboot, STUB_MEMCPY, STUB_MEMSET, STUB_STACK_CHK_FAIL, GOT_STACK_CHK_GUARD)
        if rc:
            raise RuntimeError(f"ps4o_init failed: {rc}")
        self.base = dll.ps4o_base()
        self._keep = []

    def _alloc(self, n, data=None):
        buf = ctypes.create_string_buffer(max(n, 1))
        if data:
            ctypes.memmove(buf, data, len(data))
        self._keep.append(buf)
        return ctypes.addressof(buf)

    def _call(self, fn, *args):
        a = (ctypes.c_uint64 * max(len(args), 6))(*[x & 0xFFFFFFFFFFFFFFFF for x in args])
        ret, fault = ctypes.c_uint64(), ctypes.c_uint64()
        code = self.dll.ps4o_call(fn, a, len(args), ctypes.byref(ret), ctypes.byref(fault))
        if code:
            raise RuntimeError(f"exception {code:#x} at eboot+{fault.value:#x}")
        return ret.value & 0xFFFFFFFF

    def gnm_format(self, g1t_fmt):
        return struct.unpack("<I", ctypes.string_at(self.base + FORMAT_TABLE + 4 * g1t_fmt, 4))[0]

    def kt_layout(self, w, h, mips, g1t_fmt, layers=1):
        """KTGL's texture setup as the G1T loader calls it. mips must not
        exceed the real chain length. pitch_bytes is pitch_pixels * bpp / 8,
        so for BC formats it is in pixels, not blocks."""
        self._keep.clear()
        out = self._alloc(888)
        self._call(FN_KT_LAYOUT, w, h, layers, mips, g1t_fmt, 0,
                   0, 0, 0, out, 0, 0, 0, 0, 0)
        raw = ctypes.string_at(out, 888)
        u = lambda o: struct.unpack_from("<I", raw, o)[0]
        res = {"total": u(0), "gnm_format": u(32), "tile_mode": u(36), "mip_count": u(52), "mips": []}
        for i in range(res["mip_count"]):
            b = 56 + 52 * i
            res["mips"].append({"size": u(b), "pitch_bytes": u(b + 36),
                                "w": u(b + 40), "h": u(b + 44), "layers": u(b + 48)})
        return res

    def kt_volume_layout(self, w, h, depth, mips, g1t_fmt):
        """KTGL's volume setup: per-mip size, size, and the tile mode
        (Thick_1dThick for tiled formats except 16-byte raw texels)."""
        self._keep.clear()
        out = self._alloc(888)
        self._call(FN_KT_VOLUME, w, h, depth, mips, g1t_fmt, 0, out)
        raw = ctypes.string_at(out, 888)
        u = lambda o: struct.unpack_from("<I", raw, o)[0]
        return {"total": u(0), "tile_mode": u(36), "mip_count": u(52),
                "mips": [u(56 + 52 * i) for i in range(u(52))]}

    def tile(self, src, *, tile_mode, w, h, bits_per_pixel, bc=False, depth=1,
             mip=0, slice_=0, surface_flags=0, dst_size=None):
        """linear -> tiled via GpuAddress tileSurface. w/h in pixels;
        bits_per_pixel is per pixel even for BC (4 for BC1, 8 for BC3).
        surface_flags 0x20 marks a volume (Gnm texture type 3D), without
        which `depth` is ignored."""
        self._keep.clear()
        # TilingParameters: tileMode, minGpuMode, linearWidth, linearHeight,
        # linearDepth, numFragmentsPerPixel, baseTiledPitch, mipLevel,
        # arraySlice, surfaceFlags, bitsPerFragment(u16), pad, isBlockCompressed,
        # tileSwizzleMask, pad
        tp = struct.pack("<IIIIIIIIIIHHBBH", tile_mode, 0, w, h, depth, 1, 0, mip, slice_, surface_flags,
                         bits_per_pixel, 0, 1 if bc else 0, 0, 0)
        tp_p = self._alloc(len(tp), tp)
        src_p = self._alloc(len(src), src)
        n = dst_size or len(src) * 4 + 0x10000
        dst_p = self._alloc(n)
        rc = self._call(FN_TILE, dst_p, src_p, tp_p)
        if rc:
            raise RuntimeError(f"tileSurface failed: {rc:#x}")
        return ctypes.string_at(dst_p, n)


# -- reference rules the verify pass checks the oracle against ---------------
def _al(v, a):
    return (v + a - 1) // a * a


def thin1d_size(ew, eh, eb):
    """Thin_1dThin: 8x8-element Morton micro-tiles, row-major; pitch is
    align8(w), grown by 8 until one slice is a multiple of 256 bytes."""
    pw, ph = _al(ew, 8), _al(eh, 8)
    while (pw * ph * eb) % 256:
        pw += 8
    return pw, pw * ph * eb


def linear_aligned_size(w, h, eb):
    """Display_LinearAligned: pitch aligned to 64 bytes, grown by that unit
    until one slice is a multiple of 256 bytes."""
    a = max(1, 64 // eb)
    pw = _al(w, a)
    while (pw * h * eb) % 256:
        pw += a
    return pw, pw * h * eb


def morton8(x, y):
    return ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))


def verify(o):
    fails = 0
    # (label, tiled G1T format, bits per pixel, bc, texc format name or None)
    tiled = [("R8", 0x18, 8, False, "RAW8"), ("RG8", 0x17, 16, False, "RAW16"),
             ("RGBA8", 0x09, 32, False, "RGBA8"), ("RGBA16", 0x0C, 64, False, "RAW64"),
             ("RGBA32F", 0x0D, 128, False, "RAW128"), ("BC1", 0x06, 4, True, "BC1"),
             ("BC3", 0x08, 8, True, "BC3"), ("BC4", 0x5C, 4, True, "BC4"), ("BC7", 0x5F, 8, True, "BC7")]
    rng = random.Random(1)
    sizes = [(1, 1), (4, 4), (8, 8), (16, 16), (37, 23), (64, 32), (100, 60), (256, 128), (1000, 600)]
    sizes += [(rng.randint(1, 700), rng.randint(1, 700)) for _ in range(12)]
    tmp = tempfile.mkdtemp(prefix="ps4o_")
    checks = 0
    for label, fmt, bpp, bc, texc_name in tiled:
        eb = bpp * 2 if bc else bpp // 8
        for (w, h) in sizes:
            ew, eh = ((w + 3) // 4, (h + 3) // 4) if bc else (w, h)
            m = o.kt_layout(w, h, 1, fmt)["mips"][0]
            pw, size = thin1d_size(ew, eh, eb)
            if size != m["size"]:
                print(f"FAIL size {label} {w}x{h}: oracle {m['size']} rule {size}")
                fails += 1
                continue
            n = ew * eh
            src = b"".join(i.to_bytes(4, "little").ljust(eb, b"\xee")[:eb] if eb >= 4
                           else (i & ((1 << (8 * eb)) - 1)).to_bytes(eb, "little") for i in range(n))
            ref = o.tile(src, tile_mode=13, w=w, h=h, bits_per_pixel=bpp, bc=bc, dst_size=size)
            for i in range(n):
                x, y = i % ew, i // ew
                t = ((y >> 3) * (pw >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)
                if ref[t * eb:(t + 1) * eb] != src[i * eb:(i + 1) * eb]:
                    print(f"FAIL map {label} {w}x{h} element ({x},{y})")
                    fails += 1
                    break
            checks += 1
            if texc_name and os.path.exists(TEXC):
                rnd = rng.randbytes(n * eb)
                ref = o.tile(rnd, tile_mode=13, w=w, h=h, bits_per_pixel=bpp, bc=bc, dst_size=size)
                lin, til = os.path.join(tmp, "lin.bin"), os.path.join(tmp, "til.bin")
                open(lin, "wb").write(rnd)
                r = subprocess.run([TEXC, "reswizzle", "-i", lin, "-f", texc_name, "-w", str(w), "-h", str(h),
                                    "-m", "ps4", "-o", til], capture_output=True, text=True)
                if r.returncode or open(til, "rb").read() != ref:
                    print(f"FAIL texc {label} {w}x{h}")
                    fails += 1
                checks += 1
    linear = [("L8", 0x0F, 1), ("L16", 0x14, 2), ("L32", 0x00, 4)]
    for label, fmt, eb in linear:
        for (w, h) in sizes:
            m = o.kt_layout(w, h, 1, fmt)["mips"][0]
            if linear_aligned_size(w, h, eb)[1] != m["size"]:
                print(f"FAIL linear {label} {w}x{h}: oracle {m['size']} rule {linear_aligned_size(w, h, eb)[1]}")
                fails += 1
            checks += 1
    cc, cf = verify_chains(o, tmp, rng)
    checks += cc
    fails += cf
    vc, vf = verify_volumes(o, tmp, rng)
    checks += vc
    fails += vf
    print(f"{checks} checks, {fails} failed")
    return fails


def verify_chains(o, tmp, rng):
    """2D mip chains at any size: texc's PS4 surface layout must match
    KTGL's per-mip sizes (GNM pads a chain's mips to powers of two)."""
    fails = checks = 0
    fmts = [("R8", 0x18, "RAW8"), ("RG8", 0x17, "RAW16"), ("RGBA8", 0x09, "RGBA8"),
            ("RGBA16", 0x0C, "RAW64"), ("RGBA32F", 0x0D, "RAW128"), ("BC1", 0x06, "BC1"),
            ("BC7", 0x5F, "BC7")]
    for _ in range(80):
        label, fmt, texc_name = rng.choice(fmts)
        w, h = rng.randint(1, 600), rng.randint(1, 600)
        mips = rng.randint(2, max(w, h).bit_length())
        L = o.kt_layout(w, h, mips, fmt)
        r = subprocess.run([TEXC, "info", "-f", texc_name, "-w", str(w), "-h", str(h), "-m", "ps4",
                            "--mips", str(len(L["mips"]))], capture_output=True, text=True)
        sizes = [int(line.split()[-1]) for line in r.stdout.splitlines()
                 if line.strip()[:1].isdigit() and len(line.split()) >= 5]
        checks += 1
        want = [m["size"] for m in L["mips"]]
        if r.returncode or sizes != want:
            print(f"FAIL chain {label} {w}x{h} mips {mips}: texc {sizes} oracle {want}")
            fails += 1
    return checks, fails


def verify_volumes(o, tmp, rng):
    """Volume (G1T type 2) textures: texc's PS4 volume layout total must
    equal KTGL's, and texc's tiled mip 0 must equal tileSurface's."""
    fails = checks = 0
    # (label, G1T format, bits per pixel, bc, texc format, element bytes)
    fmts = [("R8", 0x18, 8, False, "RAW8", 1), ("RG8", 0x17, 16, False, "RAW16", 2),
            ("RGBA8", 0x09, 32, False, "RGBA8", 4), ("RGBA16", 0x0C, 64, False, "RAW64", 8),
            ("RGBA32F", 0x0D, 128, False, "RAW128", 16), ("BC1", 0x06, 4, True, "BC1", 8),
            ("BC7", 0x5F, 8, True, "BC7", 16)]
    sizes = [(32, 32, 32, 1), (20, 12, 6, 1), (5, 3, 3, 1), (100, 60, 10, 1), (64, 64, 16, 1),
             (16, 16, 16, 5), (32, 32, 32, 6), (64, 32, 8, 4)]
    for label, fmt, bpp, bc, texc_name, eb in fmts:
        for (w, h, d, mips) in sizes:
            L = o.kt_volume_layout(w, h, d, mips, fmt)
            out = os.path.join(tmp, "vol.bin")
            ew, eh = ((w + 3) // 4, (h + 3) // 4) if bc else (w, h)
            lin = rng.randbytes(ew * eh * d * eb)
            src = os.path.join(tmp, "vlin.bin")
            open(src, "wb").write(lin)
            r = subprocess.run([TEXC, "reswizzle", "-i", src, "-f", texc_name, "-w", str(w), "-h", str(h),
                                "-m", "ps4", "--mips", str(mips), "--depth", str(d), "--mip", "0",
                                "-o", out], capture_output=True, text=True)
            checks += 1
            if r.returncode:
                print(f"FAIL volume {label} {w}x{h}x{d} mips {mips}: texc {r.stderr.strip()}")
                fails += 1
                continue
            got = open(out, "rb").read()
            if len(got) != L["total"]:
                print(f"FAIL volume size {label} {w}x{h}x{d} mips {mips}: texc {len(got)} oracle {L['total']}")
                fails += 1
                continue
            checks += 1
            ref = o.tile(lin, tile_mode=L["tile_mode"], w=w, h=h, bits_per_pixel=bpp, bc=bc,
                         depth=d, surface_flags=0x20, dst_size=L["mips"][0])
            if got[:L["mips"][0]] != ref:
                print(f"FAIL volume tiling {label} {w}x{h}x{d} (tile mode {L['tile_mode']})")
                fails += 1
    return checks, fails


# (texc format, G1T format, bits per pixel, bc, element bytes, w, h, depth, mips)
VOLUME_KAT = [
    ("RAW64", 0x0C, 64, False, 8, 32, 32, 32, 1),     # Nioh 2 colour LUT
    ("RAW32", 0x0A, 32, False, 4, 16, 16, 16, 1),
    ("RAW64", 0x0C, 64, False, 8, 32, 128, 256, 1),
    ("RAW8", 0x18, 8, False, 1, 100, 60, 10, 1),
    ("RAW16", 0x17, 16, False, 2, 33, 17, 5, 3),
    ("RGBA8", 0x09, 32, False, 4, 20, 12, 6, 2),
    ("RAW128", 0x0D, 128, False, 16, 48, 32, 7, 1),   # RGBA32F: thin per slice
    ("BC1", 0x06, 4, True, 8, 64, 64, 16, 4),         # BC: thin per slice
    ("BC7", 0x5F, 8, True, 16, 256, 256, 64, 7),      # Nioh 2 0x64-shaped
    ("BC7", 0x5F, 8, True, 16, 40, 24, 5, 1),
]


def np2(v):
    n = 1
    while n < v:
        n <<= 1
    return n


def kat_fill(n, seed):
    s, m, out = seed or 1, 0xFFFFFFFFFFFFFFFF, bytearray(n)
    for i in range(n):
        s ^= s >> 12
        s ^= (s << 25) & m
        s ^= s >> 27
        out[i] = ((s * 0x2545F4914F6CDD1D) & m) >> 56
    return bytes(out)


def fnv(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def kat(o):
    """Known answers for tests/ps4_volume_kat.h: KTGL's volume size and
    the GpuAddress-tiled bytes of mip 0 (kat_fill input, FNV-1a 64)."""
    rows = []
    for i, (tf, fmt, bpp, bc, eb, w, h, d, mips) in enumerate(VOLUME_KAT):
        L = o.kt_volume_layout(w, h, d, mips, fmt)
        ew, eh = ((w + 3) // 4, (h + 3) // 4) if bc else (w, h)
        seed = (0x243F6A8885A308D3 + 0x9E3779B97F4A7C15 * (2001 + i)) & 0xFFFFFFFFFFFFFFFF
        lin = kat_fill(ew * eh * d * eb, seed)
        # in a mip chain GNM pads mip 0 to powers of two: tile the padded
        # volume with the image in its corner and zeros around it
        pw, ph, pd = (np2(w), np2(h), np2(d)) if mips > 1 else (w, h, d)
        pew, peh = ((pw + 3) // 4, (ph + 3) // 4) if bc else (pw, ph)
        padded = bytearray(pew * peh * pd * eb)
        for z in range(d):
            for y in range(eh):
                a = ((z * eh + y) * ew) * eb
                b = ((z * peh + y) * pew) * eb
                padded[b:b + ew * eb] = lin[a:a + ew * eb]
        tiled = o.tile(bytes(padded), tile_mode=L["tile_mode"], w=pw, h=ph, bits_per_pixel=bpp, bc=bc,
                       depth=pd, surface_flags=0x20, dst_size=L["mips"][0])
        rows.append(f"    {{ TEXC_FORMAT_{tf}, {w}, {h}, {d}, {mips}, {L['total']}ull, "
                    f"{L['mips'][0]}ull, {seed:#018x}ull, {fnv(tiled):#018x}ull }},")
    print("""/*
 * ps4_volume_kat.h - PS4 volume (G1T type 2) known answers, generated by
 * `tools/ps4_oracle/ps4_oracle.py kat` from Sony's GpuAddress code inside a
 * KTGL eboot: the whole volume's size from KTGL's own volume setup, and the
 * hash of mip 0 tiled by tileSurface from kat_fill(seed) input.
 */
#ifndef TEXC_PS4_VOLUME_KAT_H
#define TEXC_PS4_VOLUME_KAT_H

#include <stdint.h>
#include "../include/tex_codec.h"

struct ps4_volume_kat {
    texc_format format;
    uint32_t w, h, depth, mips;
    uint64_t total_size, mip0_size;
    uint64_t seed;
    uint64_t hash;
};

static const ps4_volume_kat k_ps4_volume_kat[] = {""")
    print("\n".join(rows))
    print("""};

#endif /* TEXC_PS4_VOLUME_KAT_H */""")
    return 0


def main(argv):
    o = Oracle()
    if len(argv) >= 6 and argv[1] == "layout":
        w, h, mips, fmt = int(argv[2], 0), int(argv[3], 0), int(argv[4], 0), int(argv[5], 0)
        layers = int(argv[6], 0) if len(argv) > 6 else 1
        L = o.kt_layout(w, h, mips, fmt, layers)
        print(f"g1t {fmt:#04x} gnm {L['gnm_format']:#010x} tile mode {L['tile_mode']} "
              f"({TILE_MODE_NAMES.get(L['tile_mode'], '?')}) total {L['total']}")
        for i, m in enumerate(L["mips"]):
            print(f"  mip{i} {m['w']}x{m['h']} size {m['size']} pitch_bytes {m['pitch_bytes']}")
        return 0
    if len(argv) >= 2 and argv[1] == "verify":
        return 1 if verify(o) else 0
    if len(argv) >= 2 and argv[1] == "kat":
        return kat(o)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
