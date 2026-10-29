# ps4_oracle

Checks the PS4 path of tex_codec against Sony's own GpuAddress code, by
running the copy that is statically linked into a KTGL PS4 game eboot
(Stranger of Paradise). The eboot is a plain x86-64 SysV ELF, so
`ps4_host.dll` maps it into the process, patches its `memcpy` / `memset` /
`__stack_chk_fail` import stubs and the `__stack_chk_guard` GOT slot, and
calls into it through a Windows-to-SysV thunk (`sysv_thunk.asm`). It runs
natively because the code uses AVX, which Unicorn can't emulate.

Nothing from the eboot is copied into this repository, and the game binary is
never committed. The oracle runs it as a black box, the same way
`tools/ps5_oracle` uses the PS5 SDK DLL.

## Build (Windows, MSVC x64)

From a VS x64 developer prompt in the repo root:

```bat
ml64 /nologo /c /Fo build\sysv_thunk.obj tools\ps4_oracle\sysv_thunk.asm
cl /nologo /O2 /EHa /LD /MD tools\ps4_oracle\ps4_host.cpp build\sysv_thunk.obj ^
   /Fo:build\ps4_host.obj /Fe:build\ps4_host.dll
```

## Use

```bat
set TEXC_PS4_EBOOT=C:\path\to\eboot-SOP.bin
python tools\ps4_oracle\ps4_oracle.py verify
python tools\ps4_oracle\ps4_oracle.py layout 128 16 1 0x0c 285
```

`layout` takes the G1T dimensions, mip count, G1T format byte and layer count.
It prints the tile mode KTGL picks and the size of each mip. When the G1T
system byte is `0x0B` (PS4), this is also the exact layout of the file's
data: the loader reads each mip straight into GPU memory. Pass the texture's
real mip count, because a count longer than the chain gives garbage.

`verify` checks three things:

- The tiler: every element size from 8 to 128 bits and BC1/3/4/7 must map to
  8x8 Morton micro-tiles, laid out row by row.
- Both pitch and size rules (below).
- When `build\Release\texc.exe` exists, `texc reswizzle -m ps4` output is
  compared byte for byte with the oracle.

It must report 0 failed.

## What it established

- KTGL picks the tile mode from the G1T format number alone. Each "linear"
  format (0x00-0x04, 0x0E, 0x0F, 0x14, ...) gets tile mode 8,
  `Display_LinearAligned`. Its tiled twin with the same Gnm format
  (0x09-0x0D, 0x17, 0x18, ...) gets mode 13, `Thin_1dThin`. BC formats always
  get mode 13, and depth formats get mode 5.
- `Thin_1dThin` uses 8x8-element Morton micro-tiles in row-major order for
  every element size. The pitch is `align8(w)`, then grows by 8 until one
  slice (`pitch * align8(h) * bytes`) is a multiple of 256 bytes. That only
  changes anything for 1- and 2-byte texels.
- `Display_LinearAligned` aligns the pitch to 64 bytes, then grows it by that
  unit until `pitch * h * bytes` is a multiple of 256.
- Power-of-two mip chains are each level at its own size, stored back to
  back. Non-power-of-two chains with more than one mip are padded up
  further, and that rule is not pinned down yet: use `tile(..., mip=i)` or
  `layout` per level.
