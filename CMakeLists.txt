cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------- version
# include/tex_codec.h is the SINGLE SOURCE OF TRUTH: parse the version out
# of it so the build, the installed package and the Windows file-version
# resource can never drift from the header.
set(TEXC_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/tex_codec.h")
file(READ "${TEXC_VERSION_HEADER}" _texc_header_text)
foreach(_part MAJOR MINOR PATCH)
    if(NOT _texc_header_text MATCHES "#define TEXC_VERSION_${_part}[ \t]+([0-9]+)")
        message(FATAL_ERROR
            "Cannot find TEXC_VERSION_${_part} in ${TEXC_VERSION_HEADER}")
    endif()
    set(TEXC_VER_${_part} "${CMAKE_MATCH_1}")
endforeach()
set(TEXC_VERSION "${TEXC_VER_MAJOR}.${TEXC_VER_MINOR}.${TEXC_VER_PATCH}")

project(tex_codec VERSION ${TEXC_VERSION} LANGUAGES CXX)
message(STATUS "tex_codec version ${TEXC_VERSION} (from include/tex_codec.h)")

# Short git hash for build tracking; "unknown" outside a checkout. Captured
# at configure time - re-run cmake to refresh it after committing.
set(TEXC_GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE TEXC_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _texc_git_dirty
        ERROR_QUIET)
    if(NOT _texc_git_dirty STREQUAL "")
        set(TEXC_GIT_HASH "${TEXC_GIT_HASH}-dirty")
    endif()
    if(TEXC_GIT_HASH STREQUAL "")
        set(TEXC_GIT_HASH "unknown")
    endif()
endif()
message(STATUS "tex_codec build id: git ${TEXC_GIT_HASH}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Stamp the parsed version into a target's Windows file-version resource,
# so the .dll / .exe report it in Explorer -> Properties -> Details.
# No-op on non-Windows targets.
if(WIN32 AND NOT EMSCRIPTEN)
    enable_language(RC)
endif()

function(texc_add_version_resource target filetype description filename)
    if(NOT WIN32 OR EMSCRIPTEN)
        return()
    endif()
    set(TEXC_RC_FILETYPE      "${filetype}")
    set(TEXC_RC_DESCRIPTION   "${description}")
    set(TEXC_RC_FILENAME      "${filename}")
    set(TEXC_RC_INTERNAL_NAME "${target}")
    set(TEXC_RC_COMPANY       "")
    set(TEXC_RC_COPYRIGHT     "")
    set(_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
                   "${_rc}" @ONLY)
    target_sources(${target} PRIVATE "${_rc}")
endfunction()

option(TEXC_BUILD_SHARED "Build tex_codec as a shared library" OFF)
option(TEXC_BUILD_TESTS  "Build the test suite"                ON)
option(TEXC_BUILD_CLI    "Build the texc command line tool"    ON)

set(TEXC_SOURCES
    src/tex_codec.cpp
    src/codecs/common.cpp
    src/codecs/bcn.cpp
    src/codecs/etc.cpp
    src/codecs/astc.cpp
    src/codecs/pvrtc.cpp
    src/codecs/atc.cpp
    src/codecs/wii.cpp
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
    target_compile_definitions(tex_codec PRIVATE
        TEXC_GIT_HASH="${TEXC_GIT_HASH}"
        TEXC_BUILD_CONFIG="wasm")

    set(TEXC_WASM_EXPORTS
        _texc_version _texc_version_string _texc_build_info
        _texc_format_name _texc_result_str _texc_block_dims
        _texc_encoded_size _texc_decoded_size _texc_can_decode _texc_can_encode
        _texc_decode _texc_decode_f32 _texc_encode _texc_encode_ex
        _texc_encode_f32 _texc_swizzled_size _texc_unswizzled_size _texc_unswizzle _texc_swizzle
        _texc_reswizzle _texc_decode_swizzled
        _texc_profile_name _texc_profile_bytes_per_pixel _texc_convert_profile
        _texc_flip_y _texc_flip_x _texc_crop
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
        set_target_properties(tex_codec PROPERTIES
            VERSION   ${TEXC_VERSION}          # libtex_codec.so.1.1.0
            SOVERSION ${TEXC_VER_MAJOR})       # libtex_codec.so.1
        texc_add_version_resource(tex_codec VFT_DLL
            "tex_codec texture codec library" "tex_codec.dll")
    else()
        add_library(tex_codec STATIC ${TEXC_SOURCES})
    endif()
    target_include_directories(tex_codec PUBLIC include)

    if(MSVC)
        target_compile_options(tex_codec PRIVATE /W3)
    else()
        target_compile_options(tex_codec PRIVATE -Wall -Wextra)
    endif()

    # Build identification compiled into texc_build_info().
    target_compile_definitions(tex_codec PRIVATE
        TEXC_GIT_HASH="${TEXC_GIT_HASH}"
        TEXC_BUILD_CONFIG="$<CONFIG>")

    # --------------------------------------------------------------- cli
    if(TEXC_BUILD_CLI)
        add_executable(texc_cli cli/texc_cli.cpp)
        target_link_libraries(texc_cli PRIVATE tex_codec)
        set_target_properties(texc_cli PROPERTIES OUTPUT_NAME texc)
        texc_add_version_resource(texc_cli VFT_APP
            "texc texture codec command line tool" "texc.exe")
        install(TARGETS texc_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    # ----------------------------------------------------------- release
    # Packages the release zip (wasm + cli + header + docs) via
    # tools/make_release.py. A dedicated target rather than a POST_BUILD
    # step, because the zip also needs the Emscripten output, which the
    # native build does not produce:
    #
    #   cmake --build build --config Release --target release
    #
    # Add --publish (see the script) to also create the GitHub release from
    # the matching CHANGELOG.md section.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_custom_target(release
            COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_release.py"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Packaging tex_codec release zip"
            USES_TERMINAL)
        add_dependencies(release tex_codec)
        if(TARGET texc_cli)
            add_dependencies(release texc_cli)
        endif()
    endif()

    # ------------------------------------------------------------- tests
    if(TEXC_BUILD_TESTS)
        enable_testing()
        add_executable(tex_codec_tests tests/test_main.cpp)
        target_link_libraries(tex_codec_tests PRIVATE tex_codec)
        # Run from the source tree so the version test can read
        # wasm/tex_codec_api.mjs and check it against the header.
        add_test(NAME roundtrip COMMAND tex_codec_tests
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()

# --------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS tex_codec
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/tex_codec.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Version file so consumers can require a version:
#   find_package(tex_codec 1.1 REQUIRED)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/tex_codecConfigVersion.cmake"
    VERSION ${TEXC_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tex_codecConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tex_codec")
