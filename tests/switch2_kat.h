/*
 * switch2_kat.h - Switch (Tegra X1) and Switch 2 (Orin) block-linear
 * known-answer table.
 *
 * Generated from ScanMountGoat's tegra_swizzle C API
 * (swizzle_block_linear, isOrin = false / true): for each case a
 * deterministic linear buffer (kat_fill with `seed`, see
 * tools/ps5_oracle/kat_common.h) is swizzled by the library and the tiled
 * output hashed (FNV-1a 64). tests/test_main.cpp reswizzles the same input
 * with tex_codec and compares. `arg` is log2 of the block height in GOBs,
 * as for texc_swizzle.
 */
#ifndef TEXC_SWITCH2_KAT_H
#define TEXC_SWITCH2_KAT_H

#include <stdint.h>
#include "../include/tex_codec.h"

struct switch2_kat {
    texc_swizzle_mode mode;
    texc_format format;
    uint32_t w, h, arg;
    uint64_t tiled_size;
    uint64_t seed;
    uint64_t hash;
};

static const switch2_kat k_switch2_kat[] = {
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_RGBA8, 16, 8, 0, 512ull, 0xcb2a60db3fe236f0ull, 0xc673ee0a3252f075ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_RGBA8, 16, 8, 0, 512ull, 0x6961da94bf2cb305ull, 0x986306e67e6fdce6ull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_RGBA8, 256, 256, 4, 262144ull, 0x0799544e3e772f1aull, 0xe43cb5d62cb07578ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_RGBA8, 256, 256, 4, 262144ull, 0xa5d0ce07bdc1ab2full, 0xc13e00627b0c1233ull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_RGBA8, 100, 60, 2, 28672ull, 0x440847c13d0c2744ull, 0x9a99f19f33d50650ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_RGBA8, 100, 60, 2, 28672ull, 0xe23fc17abc56a359ull, 0xb75fdb4e78fcb232ull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_RGBA8, 37, 23, 0, 4608ull, 0x80773b343ba11f6eull, 0x71b2bb9c526c90daull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_RGBA8, 37, 23, 0, 4608ull, 0x1eaeb4edbaeb9b83ull, 0xc8431c47bc329c73ull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC1, 256, 256, 3, 32768ull, 0xbce62ea73a361798ull, 0xe04a68df7cc5b98aull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_BC1, 256, 256, 3, 32768ull, 0x5b1da860b98093adull, 0x6bd64b20585d513full },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC1, 1024, 64, 1, 32768ull, 0xf955221a38cb0fc2ull, 0x8a13a14bf4d9ebb3ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_BC1, 1024, 64, 1, 32768ull, 0x978c9bd3b8158bd7ull, 0xcfa9c1babafb7d32ull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC1, 60, 44, 0, 2048ull, 0x35c4158d376007ecull, 0xc74e3857f38cce40ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_BC1, 60, 44, 0, 2048ull, 0xd3fb8f46b6aa8401ull, 0xdd01310e537c7e2dull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC7, 512, 512, 4, 262144ull, 0x7233090035f50016ull, 0x3c7ee411481137f5ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_BC7, 512, 512, 4, 262144ull, 0x106a82b9b53f7c2bull, 0x9e028d6b7b556fa1ull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC7, 256, 128, 2, 32768ull, 0xaea1fc733489f840ull, 0xa9e4beccb9505f9aull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_BC7, 256, 128, 2, 32768ull, 0x4cd9762cb3d47455ull, 0xdc570bc21ba97458ull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC7, 300, 70, 1, 38912ull, 0xeb10efe6331ef06aull, 0xf5995759e8ddf6d6ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_BC7, 300, 70, 1, 38912ull, 0x8948699fb2696c7full, 0x0df54d627a6a6f3bull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_BC7, 2048, 2048, 5, 4194304ull, 0x277fe35931b3e894ull, 0x89874e9bb9c54d94ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_BC7, 2048, 2048, 5, 4194304ull, 0xc5b75d12b0fe64a9ull, 0xa574e1250c829628ull },
    { TEXC_SWIZZLE_SWITCH, TEXC_FORMAT_ASTC_8x8, 640, 360, 3, 81920ull, 0x63eed6cc3048e0beull, 0xd7058311cbf95f98ull },
    { TEXC_SWIZZLE_SWITCH2, TEXC_FORMAT_ASTC_8x8, 640, 360, 3, 81920ull, 0x02265085af935cd3ull, 0x2460655a11b02680ull },
};

#endif /* TEXC_SWITCH2_KAT_H */
