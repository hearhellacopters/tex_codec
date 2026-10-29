"""PS4 tiling oracle.

Runs Sony's GpuAddress code, statically linked into a KTGL PS4 eboot
(Stranger of Paradise, eboot.bin), natively through ps4_host.dll. Nothing
from the eboot is copied; it is executed as a black box, the same way
tools/ps5_oracle checks against the PS5 SDK DLL.

Entry points (eboot-SOP.bin, image base 0):
  0x22B4280  KTGL 2D/array texture setup: G1T format index -> tile mode,
             per-mip size and pitch (the same call the G1T loader makes,
             which is also the on-disk layout when the G1T system is 0x0B)
  0x1DA70    GpuAddress tileSurface(dst, src, const TilingParameters*)

Usage:
  python tools/ps4_oracle/ps4_oracle.py layout <w> <h> <mips> <g1t_fmt> [layers]
  python tools/ps4_oracle/ps4_oracle.py verify
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
FN_TILE = 0x1DA70
FORMAT_TABLE = 0x2FC9E80
STUB_MEMCPY = 0x2C6FA70
STUB_MEMSET = 0x2C71720
STUB_STACK_CHK_FAIL = 0x2C6FA60
GOT_STACK_CHK_GUARD = 0x36177C8

# GpuAddress tile modes KTGL picks for textures
TILE_MODE_NAMES = {5: "Depth_1dThin", 8: "Display_LinearAligned", 13: "Thin_1dThin"}


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

    def tile(self, src, *, tile_mode, w, h, bits_per_pixel, bc=False, depth=1,
             mip=0, slice_=0, dst_size=None):
        """linear -> tiled via GpuAddress tileSurface. w/h in pixels;
        bits_per_pixel is per pixel even for BC (4 for BC1, 8 for BC3)."""
        self._keep.clear()
        # TilingParameters: tileMode, minGpuMode, linearWidth, linearHeight,
        # linearDepth, numFragmentsPerPixel, baseTiledPitch, mipLevel,
        # arraySlice, surfaceFlags, bitsPerFragment(u16), pad, isBlockCompressed,
        # tileSwizzleMask, pad
        tp = struct.pack("<IIIIIIIIIIHHBBH", tile_mode, 0, w, h, depth, 1, 0, mip, slice_, 0,
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
    tiled = [("R8", 0x18, 8, False, None), ("RG8", 0x17, 16, False, None),
             ("RGBA8", 0x09, 32, False, "RGBA8"), ("RGBA16", 0x0C, 64, False, None),
             ("RGBA32F", 0x0D, 128, False, None), ("BC1", 0x06, 4, True, "BC1"),
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
    print(f"{checks} checks, {fails} failed")
    return fails


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
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
