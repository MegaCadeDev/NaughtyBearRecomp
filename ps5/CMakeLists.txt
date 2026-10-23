# The PS5 version: compiles the recompiled game, the runtime and their libraries with the
# PS5 payload SDK's toolchain into static libraries. ps5/tools/build.sh configures this, then
# links them with RADV into the title's eboot.bin and signs it (a native title, not a
# payload, so the final link isn't CMake's). No mods, mods menu or shader translator: the
# shader cache comes from the PC version.
cmake_minimum_required(VERSION 3.20)
project(NaughtyBearPS5 C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GENERATED_DIR ${ROOT}/generated/ppc)
set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/.deps)
set(PACBREW ${DEPS}/pacbrew/v0.40.2/sysroot/user/homebrew)
set(XENON_THIRDPARTY ${ROOT}/tools/XenonRecomp/thirdparty)
set(PLUME ${ROOT}/thirdparty/plume)

if (NOT EXISTS ${GENERATED_DIR}/ppc_func_mapping.cpp)
    message(FATAL_ERROR "No recompiled code in generated/ppc. Run tools/recompile.sh first.")
endif()

add_compile_options(-O2 -ffunction-sections -fdata-sections -fno-strict-aliasing -fexceptions -funwind-tables
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-deprecated-declarations
    -Wno-null-arithmetic -Wno-null-conversion -Wno-format -Wno-switch -Wno-invalid-offsetof)
add_compile_definitions(NDEBUG NOMINMAX)

# The recompiled game code.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS ${GENERATED_DIR}/ppc_recomp.*.cpp)
add_library(nb_recomp STATIC ${RECOMP_SOURCES} ${GENERATED_DIR}/ppc_func_mapping.cpp)
target_include_directories(nb_recomp PUBLIC ${GENERATED_DIR} ${XENON_THIRDPARTY}/simde ${ROOT}/tools/XenonRecomp/XenonUtils)
target_precompile_headers(nb_recomp PRIVATE ${GENERATED_DIR}/ppc_recomp_shared.h)

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
    ${ROOT}/tools/XenosRecomp/thirdparty/smol-v/source/smolv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tooling/native/zstd_trace_stubs.c)
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

# plume on Vulkan, presenting through VK_KHR_display (patches/plume/0002).
add_library(nb_plume STATIC ${PLUME}/plume_vulkan.cpp)
target_include_directories(nb_plume PUBLIC
    ${PLUME}
    ${PLUME}/contrib/volk
    ${PLUME}/contrib/Vulkan-Headers/include
    ${PLUME}/contrib/VulkanMemoryAllocator/include)
target_compile_definitions(nb_plume PUBLIC VK_NO_PROTOTYPES)

# The renderer's copy shaders, compiled to SPIR-V headers on the host.
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

# The runtime, without the PC-only parts (they compile to nothing on the PS5 anyway).
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${ROOT}/src/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "src/gpu/fallback/|src/gpu/translator/|src/mods/|src/ui/")
set(IMPORT_STUBS ${CMAKE_BINARY_DIR}/import_stubs.cpp)
add_custom_command(
    OUTPUT ${IMPORT_STUBS}
    COMMAND ${ROOT}/tools/gen_import_stubs.py ${GENERATED_DIR}/ppc_func_mapping.cpp ${IMPORT_STUBS}
    DEPENDS ${ROOT}/tools/gen_import_stubs.py ${GENERATED_DIR}/ppc_func_mapping.cpp ${RUNTIME_SOURCES})
add_library(nb_runtime STATIC ${RUNTIME_SOURCES} ${IMPORT_STUBS} ${HOST_SHADER_HEADERS})
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
