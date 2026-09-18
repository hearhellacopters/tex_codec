# ps5_oracle

Verifies `src/unswizzle/ps5_agc.cpp` bit-for-bit against Sony's
`AgcGpuAddress` host library from the PS5 SDK, and regenerates the
known-answer table `tests/ps5_kat.h` that the normal test suite checks
without needing the SDK.

The SDK is SIE confidential: it is never committed, and nothing from it is
copied into this repository. The implementation is derived from AMD's
open-source addrlib (MIT), which computes the same layout for the GFX10
"standard" swizzle modes; this tool only *checks* that.

## Build (Windows, MSVC x64)

Set `TEXC_PS5_SDK` to the SDK's `target` folder's parent (the directory
containing `host_tools/` and `target/`), build the library
(`cmake --build build --config Release`), then:

```bat
cl /nologo /EHsc /O2 /std:c++17 ^
   /I "%TEXC_PS5_SDK%\target\include_common" ^
   tools\ps5_oracle\ps5_oracle.cpp build\Release\tex_codec.lib ^
   "%TEXC_PS5_SDK%\host_tools\lib\libSceAgcGpuAddress.lib" ^
   /Fe:build\ps5_oracle.exe
copy "%TEXC_PS5_SDK%\host_tools\bin\libSceAgcGpuAddress.dll" build\
```

## Use

```bat
build\ps5_oracle.exe verify                      :: full matrix, must report 0 failed
build\ps5_oracle.exe kat > tests\ps5_kat.h       :: regenerate the KAT table
build\ps5_oracle.exe samples samples\ps5\textures  :: sample-file hashes (append to tests/ps5_kat.h)
```

`verify` covers the 256 B / 4 KB / 64 KB standard modes, 1..16-byte
elements, raw and 4x4-block formats, power-of-two and odd sizes, single
mips, partial and full chains, and multi-slice surfaces. For every case it
compares the layout (sizes, offsets, padding, tail membership and tail
coordinates), detiles every mip of every slice both ways, and finally
checks that a surface tiled by tex_codec is byte-identical to one tiled by
the SDK.
