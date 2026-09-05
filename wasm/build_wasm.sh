#!/usr/bin/env bash
# build_wasm.sh - compile tex_codec to WebAssembly with Emscripten.
#
# Prerequisites: an activated emsdk environment (emcc on PATH), e.g.
#   git clone https://github.com/emscripten-core/emsdk.git
#   ./emsdk/emsdk install latest && ./emsdk/emsdk activate latest
#   source ./emsdk/emsdk_env.sh
#
# Usage: ./build_wasm.sh [output_dir]     (default: ../dist)
#
# Produces: tex_codec.js (ES/UMD loader) + tex_codec.wasm
set -euo pipefail

cd "$(dirname "$0")"
OUT_DIR="${1:-../dist}"
mkdir -p "$OUT_DIR"

SOURCES=(
    ../src/tex_codec.cpp
    ../src/codecs/common.cpp
    ../src/codecs/bcn.cpp
    ../src/codecs/etc.cpp
    ../src/codecs/astc.cpp
    ../src/codecs/pvrtc.cpp
    ../src/codecs/atc.cpp
    ../src/codecs/wii.cpp
    ../src/unswizzle/*.cpp
    ../src/util/*.cpp
    exports.cpp
)

# Core API list; everything in wasm/exports.cpp (per-format wrappers,
# per-mode swizzles, *_alloc helpers) is additionally exported through
# EMSCRIPTEN_KEEPALIVE and needs no listing here.
# Build identification compiled into texc_build_info() - mirrors what
# CMake stamps in for native builds.
GIT_HASH="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    GIT_HASH="${GIT_HASH}-dirty"
fi

EXPORTED_FUNCTIONS='["_texc_version","_texc_version_string","_texc_build_info","_texc_format_name","_texc_result_str","_texc_block_dims","_texc_encoded_size","_texc_decoded_size","_texc_can_decode","_texc_can_encode","_texc_decode","_texc_decode_f32","_texc_encode","_texc_encode_ex","_texc_encode_f32","_texc_swizzled_size","_texc_unswizzled_size","_texc_unswizzle","_texc_swizzle","_texc_reswizzle","_texc_decode_swizzled","_texc_profile_name","_texc_profile_bytes_per_pixel","_texc_convert_profile","_texc_flip_y","_texc_flip_x","_texc_crop","_texc_alloc","_texc_free","_malloc","_free"]'

em++ "${SOURCES[@]}" \
    -I../include \
    -O3 -fno-exceptions -fno-rtti \
    -DTEXC_GIT_HASH="\"$GIT_HASH\"" -DTEXC_BUILD_CONFIG='"wasm"' \
    -s WASM=1 \
    -s EXPORTED_FUNCTIONS="$EXPORTED_FUNCTIONS" \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAPF32","getValue","setValue","UTF8ToString"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME=TexCodec \
    -s ENVIRONMENT=web,worker,node \
    -o "$OUT_DIR/tex_codec.js"

# Ship the ergonomic JS/TS wrapper next to the module so the default
# "./tex_codec.js" import inside tex_codec_api.mjs resolves.
cp tex_codec_api.mjs tex_codec_api.d.ts "$OUT_DIR/"

echo "Built: $OUT_DIR/tex_codec.js + $OUT_DIR/tex_codec.wasm"
echo "       $OUT_DIR/tex_codec_api.mjs (+ .d.ts) - ergonomic wrapper"
echo "       version $(sed -n 's/^export const VERSION = "\(.*\)";/\1/p' tex_codec_api.mjs), git $GIT_HASH"
