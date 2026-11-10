# SPDX-License-Identifier: GPL-3.0-or-later
# The NIR backend (nir_backend.cpp) builds against the pinned RADV's own Mesa headers, and its
# references to Mesa are renamed like the isolated RADV archive's (tools/isolate-radv.py), so
# they reach the NIR that RADV itself compiles with.
set(radv_work "${PORT_ROOT}/../mihawk-vulkan-review/.deps/work")
set(radv_src "${radv_work}/radv-src")
set(radv_build "${radv_work}/radv-build-ps5-release")
set(radv_symbols "${PORT_ROOT}/build/radv-isolated/symbols.map")
foreach(required "${radv_src}/src/compiler/nir/nir.h"
        "${radv_build}/src/compiler/nir/nir_builder_opcodes.h" "${radv_symbols}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "NIR backend: missing ${required} (build and isolate the pinned RADV first)")
    endif()
endforeach()
add_library(eden-nir-backend STATIC "${EDEN_PORT_DIR}/nir_backend.cpp")
target_include_directories(eden-nir-backend BEFORE PRIVATE
    "${radv_build}/include" "${radv_src}/include" "${radv_build}/src" "${radv_src}/src"
    "${radv_build}/src/compiler/nir" "${radv_src}/src/compiler/nir"
    "${radv_build}/src/compiler" "${radv_src}/src/compiler"
    "${radv_build}/src/vulkan/runtime" "${radv_src}/src/vulkan/runtime"
    "${radv_build}/src/vulkan/util" "${radv_src}/src/vulkan/util"
    "${radv_build}/src/util" "${radv_build}/src/util/format")
target_include_directories(eden-nir-backend PRIVATE "${PROJECT_SOURCE_DIR}/src")
# The configuration RADV was compiled with (its compile_commands.json), as far as headers see it.
target_compile_definitions(eden-nir-backend PRIVATE
    __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS
    AMD_NO_DRM RADV_PS5 MESA_DEBUG=0 HAVE_PTHREAD HAVE_STRUCT_TIMESPEC HAVE_UINT128
    HAVE___BUILTIN_BSWAP32 HAVE___BUILTIN_BSWAP64 HAVE___BUILTIN_CLZ HAVE___BUILTIN_CLZLL
    HAVE___BUILTIN_CTZ HAVE___BUILTIN_EXPECT HAVE___BUILTIN_FFS HAVE___BUILTIN_FFSLL
    HAVE___BUILTIN_POPCOUNT HAVE___BUILTIN_POPCOUNTLL HAVE___BUILTIN_UNREACHABLE
    HAVE___BUILTIN_TYPES_COMPATIBLE_P HAVE___BUILTIN_ADD_OVERFLOW
    HAVE_FUNC_ATTRIBUTE_CONST HAVE_FUNC_ATTRIBUTE_FLATTEN HAVE_FUNC_ATTRIBUTE_MALLOC
    HAVE_FUNC_ATTRIBUTE_PURE HAVE_FUNC_ATTRIBUTE_UNUSED HAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT
    HAVE_FUNC_ATTRIBUTE_WEAK HAVE_FUNC_ATTRIBUTE_FORMAT HAVE_FUNC_ATTRIBUTE_PACKED
    HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL HAVE_FUNC_ATTRIBUTE_ALIAS HAVE_FUNC_ATTRIBUTE_NORETURN
    HAVE_FUNC_ATTRIBUTE_COLD HAVE_FUNC_ATTRIBUTE_VISIBILITY USE_GCC_ATOMIC_BUILTINS)
# Mesa's headers are not written for Eden's warning set.
target_compile_options(eden-nir-backend PRIVATE -Wno-error -Wno-missing-field-initializers
    -Wno-sign-compare -Wno-unused-function -Wno-macro-redefined)
target_link_libraries(eden-nir-backend PRIVATE shader_recompiler common)
add_custom_command(TARGET eden-nir-backend POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" "--redefine-syms=${radv_symbols}" "$<TARGET_FILE:eden-nir-backend>"
    VERBATIM)
target_link_libraries(eden-headless PRIVATE eden-nir-backend)
# main.cpp registers the backend only when this build has one.
target_compile_definitions(eden-headless PRIVATE EDEN_PS5_NIR_BACKEND=1)
