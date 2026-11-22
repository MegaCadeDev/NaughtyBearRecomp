# The Switch 2 version: a homebrew NRO for kaiHEN's hbmenu, built with ounce-devkit
# (devkitPro's devkitA64 with libounce, the Switch 2 libnx, and OunceVK, Mesa's NVK driver).
# switch2/tools/build.sh configures this in the devkit's Docker image with libounce's CMake
# toolchain, after building DXC and FFmpeg's XMA decoder for it. No mods or mods menu.
#
# Everything is compiled with devkitA64's GCC; compat/clang_builtins.h supplies the Clang
# builtins the recompiled code uses.
cmake_minimum_required(VERSION 3.20)
project(NaughtyBearSwitch2 C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GENERATED_DIR ${ROOT}/generated/ppc)
set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/.deps)
set(XENON_THIRDPARTY ${ROOT}/tools/XenonRecomp/thirdparty)
set(PLUME ${ROOT}/thirdparty/plume)

if (NOT EXISTS ${GENERATED_DIR}/ppc_func_mapping.cpp)
    message(FATAL_ERROR "No recompiled code in generated/ppc. Run tools/recompile.sh first.")
endif()
if (NOT EXISTS ${DEPS}/ffmpeg/lib/libavcodec.a OR NOT EXISTS ${DEPS}/dxc/lib/libdxcompiler.a OR NOT EXISTS ${DEPS}/ouncevk/libouncevk_local.a)
    message(FATAL_ERROR "Missing dependencies in switch2/.deps: build with switch2/tools/build.sh.")
endif()

add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-O2;-fno-strict-aliasing;-fexceptions;-include;${CMAKE_CURRENT_SOURCE_DIR}/compat/clang_builtins.h>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-function;-Wno-deprecated-declarations>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-format;-Wno-switch;-Wno-invalid-offsetof;-Wno-unknown-pragmas;-Wno-attributes;-Wno-ignored-attributes>")
add_compile_definitions(NDEBUG NOMINMAX NB_CONSOLE)
# POSIX headers newlib lacks (sys/mman.h, dlfcn.h, ...), defined in compat/posix_extra.c.
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/compat/include)
set(NB_VERSION "1.0.0" CACHE STRING "The version in the NRO's metadata")

# The recompiled game code. Objects, not an archive: the runtime's PPC_FUNC overrides replace
# its weak definitions.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS ${GENERATED_DIR}/ppc_recomp.*.cpp)
add_library(nb_recomp OBJECT ${RECOMP_SOURCES} ${GENERATED_DIR}/ppc_func_mapping.cpp)
target_include_directories(nb_recomp PUBLIC ${GENERATED_DIR} ${XENON_THIRDPARTY}/simde ${ROOT}/tools/XenonRecomp/XenonUtils)
target_precompile_headers(nb_recomp PRIVATE ${GENERATED_DIR}/ppc_recomp_shared.h)

# zstd and smol-v decode the shader cache, as on PC.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(${ROOT}/tools/XenosRecomp/thirdparty/zstd/build/cmake ${CMAKE_BINARY_DIR}/zstd EXCLUDE_FROM_ALL)

# XenonRecomp's XEX loader and the libraries it and the runtime use.
add_library(nb_thirdparty STATIC
    ${ROOT}/tools/XenonRecomp/XenonUtils/disasm.cpp
    ${ROOT}/tools/XenonRecomp/XenonUtils/xex.cpp
    ${ROOT}/tools/XenonRecomp/XenonUtils/image.cpp
    ${ROOT}/tools/XenonRecomp/XenonUtils/xdbf_wrapper.cpp
    ${ROOT}/tools/XenonRecomp/XenonUtils/xex_patcher.cpp
    ${ROOT}/tools/XenonRecomp/XenonUtils/memory_mapped_file.cpp
    ${XENON_THIRDPARTY}/libmspack/libmspack/mspack/lzxd.c
    ${XENON_THIRDPARTY}/tiny-AES-c/aes.c
    ${XENON_THIRDPARTY}/disasm/disasm.c
    ${XENON_THIRDPARTY}/disasm/ppc-dis.c
    ${XENON_THIRDPARTY}/xxHash/xxhash.c
    ${XENON_THIRDPARTY}/fmt/src/format.cc
    ${XENON_THIRDPARTY}/fmt/src/os.cc
    ${ROOT}/thirdparty/o1heap/o1heap.c
    ${ROOT}/tools/XenosRecomp/thirdparty/smol-v/source/smolv.cpp)
target_include_directories(nb_thirdparty PUBLIC
    ${ROOT}/tools/XenonRecomp/XenonUtils
    ${XENON_THIRDPARTY}/simde
    ${XENON_THIRDPARTY}/disasm
    ${XENON_THIRDPARTY}/fmt/include
    ${XENON_THIRDPARTY}/xxHash
    ${ROOT}/thirdparty/o1heap
    ${ROOT}/tools/XenosRecomp/thirdparty/smol-v/source
    PRIVATE
    ${XENON_THIRDPARTY}/libmspack/libmspack/mspack
    ${XENON_THIRDPARTY}/tiny-AES-c
    ${XENON_THIRDPARTY}/TinySHA1)

# plume on Vulkan, presenting to the default window through VK_NN_vi_surface
# (patches/plume/0003). OunceVK is linked in, with its vk* functions made local
# (tools/prepare-ouncevk.sh) so they don't clash with volk's function pointers.
add_library(nb_plume STATIC ${PLUME}/plume_vulkan.cpp)
target_include_directories(nb_plume PUBLIC
    ${PLUME}
    ${PLUME}/contrib/volk
    ${PLUME}/contrib/Vulkan-Headers/include
    ${PLUME}/contrib/VulkanMemoryAllocator/include)
target_compile_definitions(nb_plume PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_VI_NN)

# The renderer's copy shaders, compiled to SPIR-V headers.
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(HOST_SHADER_DIR ${CMAKE_BINARY_DIR}/host_shaders)
set(HOST_SHADER_HEADERS)
foreach(shader blit.vert:g_blitVS blit.frag:g_blitPS)
    string(REPLACE ":" ";" parts ${shader})
    list(GET parts 0 file)
    list(GET parts 1 name)
    set(header ${HOST_SHADER_DIR}/${file}.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HOST_SHADER_DIR}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 --vn ${name} -o ${header} ${ROOT}/src/gpu/shaders/${file}
        DEPENDS ${ROOT}/src/gpu/shaders/${file})
    list(APPEND HOST_SHADER_HEADERS ${header})
endforeach()

# The shader translator, as the PC version builds it: XenosRecomp's recompiler and DXC
# (built by tools/build-dxc.sh), with the same HLSL header and version, so the Switch 2
# translates shaders itself and its cache matches the PC version's.
set(XENOS_DIR ${ROOT}/tools/XenosRecomp/XenosRecomp)
set(SHADER_COMMON_TEXT ${CMAKE_BINARY_DIR}/translator/shader_common_text.h)
file(READ ${XENOS_DIR}/shader_common.h SHADER_COMMON_CONTENTS)
file(GLOB TRANSLATOR_PATCHES ${ROOT}/patches/XenosRecomp/*.patch)
set(TRANSLATOR_PATCH_CONTENTS "")
foreach(patch ${TRANSLATOR_PATCHES})
    file(READ ${patch} contents)
    string(APPEND TRANSLATOR_PATCH_CONTENTS "${contents}")
endforeach()
string(SHA1 TRANSLATOR_PATCH_HASH "${TRANSLATOR_PATCH_CONTENTS}")
file(WRITE ${SHADER_COMMON_TEXT}.tmp "#pragma once\nstatic const char SHADER_COMMON_TEXT[] = R\"NBHLSL(${SHADER_COMMON_CONTENTS})NBHLSL\";\nstatic const char TRANSLATOR_PATCH_HASH[] = \"${TRANSLATOR_PATCH_HASH}\";\n")
configure_file(${SHADER_COMMON_TEXT}.tmp ${SHADER_COMMON_TEXT} COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${XENOS_DIR}/shader_common.h ${TRANSLATOR_PATCHES})

add_library(nb_shader_translator STATIC
    ${ROOT}/src/gpu/translator/shader_translator.cpp
    ${XENOS_DIR}/shader_recompiler.cpp
    ${XENOS_DIR}/dxc_compiler.cpp)
target_include_directories(nb_shader_translator PRIVATE ${XENOS_DIR} ${CMAKE_BINARY_DIR}/translator ${ROOT}/src
    ${ROOT}/tools/XenosRecomp/thirdparty/dxc-bin/inc ${ROOT}/tools/XenosRecomp/thirdparty/zstd/lib)
target_precompile_headers(nb_shader_translator PRIVATE ${XENOS_DIR}/pch.h)
target_compile_options(nb_shader_translator PRIVATE -fms-extensions)
target_link_libraries(nb_shader_translator PRIVATE nb_thirdparty)

# The runtime, without the PC-only parts.
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${ROOT}/src/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "src/gpu/fallback/|src/gpu/translator/|src/mods/|src/ui/")
set(IMPORT_STUBS ${CMAKE_BINARY_DIR}/import_stubs.cpp)
add_custom_command(
    OUTPUT ${IMPORT_STUBS}
    COMMAND python3 ${ROOT}/tools/gen_import_stubs.py ${GENERATED_DIR}/ppc_func_mapping.cpp ${IMPORT_STUBS}
    DEPENDS ${ROOT}/tools/gen_import_stubs.py ${GENERATED_DIR}/ppc_func_mapping.cpp ${RUNTIME_SOURCES})
add_library(nb_runtime OBJECT ${RUNTIME_SOURCES} ${IMPORT_STUBS} ${HOST_SHADER_HEADERS})
target_include_directories(nb_runtime PRIVATE
    ${ROOT}/src
    ${ROOT}/src/gpu
    ${ROOT}/thirdparty/unordered_dense/include
    ${ROOT}/tools/XenosRecomp/thirdparty/zstd/lib
    ${DEPS}/ffmpeg/include
    ${HOST_SHADER_DIR})
target_compile_definitions(nb_runtime PRIVATE NB_HAVE_FFMPEG)
target_link_libraries(nb_runtime PUBLIC nb_recomp nb_thirdparty nb_plume)
target_precompile_headers(nb_runtime PRIVATE ${ROOT}/src/stdafx.h)

# The program. DXC initialises itself in a constructor (DllMain) that nothing refers to, as
# it's built to be a shared library: it's kept with --undefined.
file(GLOB DXC_LIBRARIES ${DEPS}/dxc/lib/*.a)
# compat/exception_entry.s: libounce's exception entry without the check that keeps
# exceptions from the handler under kaiHEN.
add_executable(NaughtyBear $<TARGET_OBJECTS:nb_recomp> $<TARGET_OBJECTS:nb_runtime> ${CMAKE_CURRENT_SOURCE_DIR}/compat/posix_extra.c
    ${CMAKE_CURRENT_SOURCE_DIR}/compat/exception_entry.s)
target_link_options(NaughtyBear PRIVATE -Wl,--undefined=_Z7DllMainv -Wl,-Map=${CMAKE_BINARY_DIR}/NaughtyBear.map)
target_link_libraries(NaughtyBear PRIVATE
    nb_shader_translator nb_plume nb_thirdparty libzstd_static
    -Wl,--start-group ${DXC_LIBRARIES} -Wl,--end-group
    ${DEPS}/ffmpeg/lib/libavcodec.a ${DEPS}/ffmpeg/lib/libavutil.a
    ${DEPS}/ouncevk/libouncevk_local.a drm_nouveau)

nx_generate_nacp(OUTPUT NaughtyBear.nacp NAME "Naughty Bear Recompiled" AUTHOR "MegaCadeDev" VERSION "${NB_VERSION}")
nx_create_nro(NaughtyBear NACP NaughtyBear.nacp ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg)
