cmake_minimum_required(VERSION 3.16)
project(tex_codec VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TEXC_BUILD_SHARED "Build tex_codec as a shared library" OFF)
option(TEXC_BUILD_TESTS  "Build the test suite"                ON)

set(TEXC_SOURCES
    src/tex_codec.cpp
    src/codecs/common.cpp
    src/codecs/bcn.cpp
    src/codecs/etc.cpp
    src/codecs/astc.cpp
    src/codecs/pvrtc.cpp
    src/codecs/atc.cpp
    src/unswizzle/unswizzle.cpp
    src/util/image_utils.cpp
)
# Optional extra unswizzle translation units (e.g. GX2 addrlib helper).
file(GLOB TEXC_UNSWIZZLE_EXTRA CONFIGURE_DEPENDS src/unswizzle/*.cpp)
list(REMOVE_ITEM TEXC_UNSWIZZLE_EXTRA
     ${CMAKE_CURRENT_SOURCE_DIR}/src/unswizzle/unswizzle.cpp)
list(APPEND TEXC_SOURCES ${TEXC_UNSWIZZLE_EXTRA})

# ---------------------------------------------------------------- library
if(EMSCRIPTEN)
    # WASM build: a single self-contained module with the C API exported.
    add_executable(tex_codec ${TEXC_SOURCES} wasm/exports.cpp)
    target_include_directories(tex_codec PUBLIC include)
    set_target_properties(tex_codec PROPERTIES SUFFIX ".js")

    # Every extern "C" API entry point, plus malloc/free for buffer setup.
    set(TEXC_WASM_EXPORTS
        _texc_version _texc_format_name _texc_result_str _texc_block_dims
        _texc_encoded_size _texc_decoded_size _texc_can_decode _texc_can_encode
        _texc_decode _texc_decode_f32 _texc_encode _texc_encode_f32
        _texc_swizzled_size _texc_unswizzle _texc_swizzle _texc_decode_swizzled
        _texc_alloc _texc_free _malloc _free
    )
    list(JOIN TEXC_WASM_EXPORTS "," TEXC_WASM_EXPORTS_STR)

    target_link_options(tex_codec PRIVATE
        "SHELL:-s EXPORTED_FUNCTIONS=[${TEXC_WASM_EXPORTS_STR}]"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAPF32','getValue','setValue','UTF8ToString']"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=TexCodec"
        "SHELL:-s ENVIRONMENT=web,worker,node"
        "SHELL:-s SINGLE_FILE=0"
        -O3
    )
    target_compile_options(tex_codec PRIVATE -O3 -fno-exceptions -fno-rtti)
else()
    if(TEXC_BUILD_SHARED)
        add_library(tex_codec SHARED ${TEXC_SOURCES})
        target_compile_definitions(tex_codec
            PRIVATE TEXC_BUILD
            PUBLIC  TEXC_SHARED)
    else()
        add_library(tex_codec STATIC ${TEXC_SOURCES})
    endif()
    target_include_directories(tex_codec PUBLIC include)

    if(MSVC)
        target_compile_options(tex_codec PRIVATE /W3)
    else()
        target_compile_options(tex_codec PRIVATE -Wall -Wextra)
    endif()

    # ------------------------------------------------------------- tests
    if(TEXC_BUILD_TESTS)
        enable_testing()
        add_executable(tex_codec_tests tests/test_main.cpp)
        target_link_libraries(tex_codec_tests PRIVATE tex_codec)
        add_test(NAME roundtrip COMMAND tex_codec_tests)
    endif()
endif()

# --------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS tex_codec
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/tex_codec.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
