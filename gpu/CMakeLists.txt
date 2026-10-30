# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
# Where the library goes: runtime/CMakeLists.txt points this at its build directory's gpu/, and a
# standalone build of this project does the same.
if (NOT BB_GPU_OUT)
    set(BB_GPU_OUT ${CMAKE_BINARY_DIR}/gpu)
endif()
# The FSR 4 benchmark is run by hand (tools/fsr4_verify.sh): out/gpu, where those scripts look.
if (NOT BB_BENCH_OUT)
    set(BB_BENCH_OUT ${CMAKE_BINARY_DIR})
endif()

# The minimum macOS, the launcher's minimumSystemVersion and deps/'s deployment target are this
# one number: without it the SDK of the machine that builds decides. A build on macOS 26 with the
# 26 SDK recorded minos 26.0, so the app refused to start before 26.0 while its manifest said 13.0.
if (APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
endif()
project(bbgpu LANGUAGES C CXX)
# ctest (after project(), which CMake requires), for a standalone build of this project; runtime/CMakeLists.txt (which includes it) does it
# for the game.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
message(STATUS "GPU library: PGO ${BB_PGO}, LTO ${BB_LTO}")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

# deps/ before the machine's own copies. CMake imports a package's include directories as
# -isystem, and Apple's clang searches its builtin /usr/local/include *before* any -isystem path:
# the LunarG Vulkan SDK a Mac may have installed there replaced the pinned headers, and the build
# failed on that Mac only (mtl_vk_layer.mm wanted a function 1.4.357 does not have). Imported
# includes are -I here, which clang searches first, so deps/ is authoritative.
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)

if (APPLE)
    find_package(VulkanHeaders CONFIG REQUIRED)
    # A Vulkan SDK installed on the machine (the LunarG one installs into /usr/local) must never be
    # the one the build uses, as a CMake choice or through the compiler's own search order.
    get_target_property(bb_vulkan_includes Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
    string(REPLACE ";" "|" bb_deps_prefixes "${CMAKE_PREFIX_PATH}")
    foreach(bb_include IN LISTS bb_vulkan_includes)
        if (NOT bb_include MATCHES "^(${bb_deps_prefixes})/")
            message(FATAL_ERROR "Vulkan headers from ${bb_include}, not from the deps prefix "
                                "(${CMAKE_PREFIX_PATH})")
        endif()
    endforeach()
    add_library(Vulkan::Vulkan INTERFACE IMPORTED GLOBAL)
    target_link_libraries(Vulkan::Vulkan INTERFACE Vulkan::Headers)
else()
    find_package(Vulkan REQUIRED)
endif()
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
find_package(magic_enum REQUIRED)
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT APPLE)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz REQUIRED)
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation and FSR 4, native Vulkan (MIT,
# FireBurn/FSR-Vulkan): the Vulkan backend's upscalers (docs/RENDER_ADAPTER.md, decision 5). Off on
# iOS, where the Metal backend is the only one and MetalFX its upscaler.
#
# The port's changes to it (patches/) go to a copy under the build directory: the submodule keeps
# matching its commit, two builds cannot fight over one tree, and a checkout updated to a newer
# FSR-Vulkan always gets the patches of its own version.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(BB_FSR_DEFAULT OFF)
else()
    set(BB_FSR_DEFAULT ON)
endif()
option(BB_FSR "Build FSR 3.1 and FSR 4 (the Vulkan backend's upscalers)" ${BB_FSR_DEFAULT})
if (BB_FSR)
    if (APPLE)
        set(BB_FSR_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/fsr-vulkan-macos)
    else()
        set(BB_FSR_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/fsr-vulkan)
    endif()
    file(GLOB bb_fsr_patches ${BB_FSR_PATCHES}/*.patch)
    set(bb_fsr_patch_hash "")
    foreach(patch IN LISTS bb_fsr_patches)
        file(READ ${patch} text)
        string(SHA256 hash "${text}")
        string(APPEND bb_fsr_patch_hash "${hash}")
    endforeach()
    string(SHA256 bb_fsr_key "${bb_fsr_patch_hash}")
    string(SUBSTRING "${bb_fsr_key}" 0 8 bb_fsr_key)
    set(BB_FSR_COPY ${CMAKE_CURRENT_BINARY_DIR}/fsr-vulkan-patched)
    set(BB_FSR_MARKER ${BB_FSR_COPY}/.patched-${bb_fsr_key})
    if (NOT EXISTS ${BB_FSR_MARKER})
        file(REMOVE_RECURSE ${BB_FSR_COPY})
        file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr-vulkan/ DESTINATION ${BB_FSR_COPY}
             PATTERN ".git" EXCLUDE PATTERN "build" EXCLUDE)
        foreach(patch IN LISTS bb_fsr_patches)
            execute_process(COMMAND git apply --unidiff-zero ${patch}
                            WORKING_DIRECTORY ${BB_FSR_COPY} RESULT_VARIABLE bb_patch_result)
            if (bb_patch_result)
                message(FATAL_ERROR "${patch} does not apply to the FSR-Vulkan submodule")
            endif()
        endforeach()
        file(TOUCH ${BB_FSR_MARKER})
    endif()
    set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
    add_subdirectory(${BB_FSR_COPY} ${CMAKE_CURRENT_BINARY_DIR}/fsr-vulkan EXCLUDE_FROM_ALL)
endif()

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(occlusion_resolve.comp occlusion_resolve_comp)
add_host_shader(gpu_timestamp.comp gpu_timestamp_comp)
add_host_shader(indirect_guard.comp indirect_guard_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
# The Metal backend (R5, docs/NATIVE_RENDERER.md) is macOS only; added below.
list(FILTER VIDEO_CORE EXCLUDE REGEX "/renderer_metal/")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# The layer's own modules (independent of the shadPS4 code): layer/.
file(GLOB LAYER_SOURCES CONFIGURE_DEPENDS layer/*.cpp)
# Game profiles (what the translator knows about one game, out of its core): games/.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS games/*.cpp)
# The translator's own command processor (bbport's docs/TRANSLATOR_ARCHITECTURE.ru.md, step 2): cp/.
file(GLOB CP_SOURCES CONFIGURE_DEPENDS cp/*.cpp)
# iOS (proof of concept, branch ios-poc): a static library linked into the app.
if (IOS)
    set(BB_GPU_LIBRARY_TYPE STATIC)
else()
    set(BB_GPU_LIBRARY_TYPE SHARED)
endif()
add_library(bbgpu ${BB_GPU_LIBRARY_TYPE} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${LAYER_SOURCES} ${GAME_SOURCES} ${CP_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
set_target_properties(bbgpu PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${BB_GPU_OUT})
# The native Metal path may not leave a Vulkan entry point unresolved (runtime/tools/check-link.cmake,
# which the game's build points BB_LINK_CHECK at).
if (APPLE AND BB_LINK_CHECK)
    add_custom_command(TARGET bbgpu POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DBB_GPU=$<TARGET_FILE:bbgpu> -DBB_BINARIES=$<TARGET_FILE:bbgpu>
                -P ${BB_LINK_CHECK}
        COMMENT "Checking that the GPU library answers every Vulkan entry point itself"
        VERBATIM)
endif()
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
# Dear ImGui stays private to libbbgpu.so. The library is linked into bb-probe, so its exports are
# in the global symbol scope; a Vulkan layer carrying its own Dear ImGui without -Bsymbolic
# (vkShade 0.1) had its ImGui_ImplVulkan_Init bound to this copy, which called the Vulkan loader
# with the layer's down-chain physical device handle ("Invalid physicalDevice", abort).
set_source_files_properties(third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
# The menu's fonts match the launcher's: Inter (text) and Cinzel (headings), SIL OFL 1.1; DejaVu
# fills in the scripts they lack (Bitstream Vera license).
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf"
    BB_FONT_TEXT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Inter.ttf"
    BB_FONT_HEADING_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Cinzel.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Inter.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Cinzel.ttf")
# The in-game menu's texts (shim/locales/*.json) as a C++ table for shim/bbport_text.cpp.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB BB_TEXT_LOCALES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shim/locales/*.json)
set(BB_TEXT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(OUTPUT ${BB_TEXT_DIR}/bbport_text_data.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BB_TEXT_DIR}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/shim/locales/gen_text.py ${BB_TEXT_DIR}/bbport_text_data.inc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shim/locales/gen_text.py ${BB_TEXT_LOCALES} VERBATIM)
target_sources(bbgpu PRIVATE ${BB_TEXT_DIR}/bbport_text_data.inc)
add_custom_command(OUTPUT ${BB_TEXT_DIR}/bbport_trophy_data.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BB_TEXT_DIR}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/shim/gen_trophy_catalog.py ${BB_TEXT_DIR}/bbport_trophy_data.inc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shim/gen_trophy_catalog.py ${CMAKE_CURRENT_SOURCE_DIR}/shim/trophy_catalog.json VERBATIM)
target_sources(bbgpu PRIVATE ${BB_TEXT_DIR}/bbport_trophy_data.inc)
target_include_directories(bbgpu PRIVATE ${BB_TEXT_DIR})
target_include_directories(bbgpu PRIVATE . fsr4_metal shim layer games cp shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include ../src ../recomp/src)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Native arm64 (docs/ARM64_NATIVE.md): x86 cycle-count and spin-wait intrinsics for the GPU library.
if (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    target_compile_options(bbgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bbport_arm64_compat.h)
endif()
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
if (APPLE)
    target_sources(bbgpu PRIVATE shadps4/video_core/renderer_metal/mtl_vk_c_bridge.cpp)
    target_link_libraries(bbgpu PRIVATE "-framework Metal" "-framework Foundation")
    target_link_options(bbgpu PRIVATE -Wl,-undefined,dynamic_lookup)
endif()
if (APPLE AND BB_FSR)
    target_sources(bbgpu PRIVATE shadps4/video_core/renderer_vulkan/vk_fsr4_metal.mm)
    set_source_files_properties(shadps4/video_core/renderer_vulkan/vk_fsr4_metal.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")

    # FSR 4.1.x on Metal (vk_fsr4_metal.mm): its kernels and model layouts, built here into
    # out/fsr4_metal (gpu/fsr4_metal/build_assets.py) from fsr4-native-macos v2.2, DXC and
    # SPIRV-Cross in deps/ (scripts/macos/build-deps.sh). FSR 4.1 (exact-fast) is built by
    # default; FSR 4.1 Lossy (lossy2) needs its pinned GPL weights and FSR 4.1.2 Lite needs the
    # model imported from the builder's own AMD DLL (tools/fsr412_import_model.py --with-shaders).
    # Without them the missing modes report themselves unavailable.
    set(BB_FSR4_NATIVE_SRC "$ENV{BB_FSR4_NATIVE_SRC}" CACHE PATH "fsr4-native-macos tree")
    if (NOT BB_FSR4_NATIVE_SRC)
        set(BB_FSR4_NATIVE_SRC ${CMAKE_PREFIX_PATH}/share/fsr4-native-macos)
    endif()
    set(BB_FSR411_PROFILES "$ENV{BB_FSR411_PROFILES}" CACHE STRING
        "FSR 4.1 profiles to build: exact-fast (default), lossy2 (comma list)")
    if (NOT BB_FSR411_PROFILES)
        set(BB_FSR411_PROFILES "exact-fast")
    endif()
    set(BB_FSR411_LOSSY2_WEIGHTS "$ENV{BB_FSR411_LOSSY2_WEIGHTS}" CACHE PATH
        "weights_lossy2.npz for FSR 4.1 Lossy (default: fetch the pinned GPL file)")
    set(BB_FSR412_MODEL "$ENV{BB_FSR412_MODEL}" CACHE PATH
        "imported FSR 4.1.2 model (tools/fsr412_import_model.py --with-shaders)")
    set(BB_FSR4_METAL_OUT ${CMAKE_SOURCE_DIR}/out/fsr4_metal)
    # The kernels need Apple's Metal compiler: Xcode with its Metal Toolchain component
    # (xcodebuild -downloadComponent MetalToolchain). The Command Line Tools have none, so with
    # xcode-select pointing at them, Xcode's own is tried.
    set(BB_METAL_DEVELOPER_DIR "")
    execute_process(COMMAND xcrun -sdk macosx metal --version
                    RESULT_VARIABLE metal_result OUTPUT_QUIET ERROR_QUIET)
    if (NOT metal_result EQUAL 0 AND EXISTS /Applications/Xcode.app/Contents/Developer)
        execute_process(COMMAND ${CMAKE_COMMAND} -E env
                                DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
                                xcrun -sdk macosx metal --version
                        RESULT_VARIABLE metal_result OUTPUT_QUIET ERROR_QUIET)
        if (metal_result EQUAL 0)
            set(BB_METAL_DEVELOPER_DIR /Applications/Xcode.app/Contents/Developer)
        endif()
    endif()
    if (NOT metal_result EQUAL 0)
        message(WARNING "No Metal compiler (Xcode and its Metal Toolchain: xcodebuild "
                        "-downloadComponent MetalToolchain): FSR 4.1.x on Metal is not built")
    elseif (EXISTS ${BB_FSR4_NATIVE_SRC}/tools/compile_fsr411.py AND EXISTS ${BB_FSR4_NATIVE_SRC}/tools/bin/dxc)
        file(GLOB BB_FSR4_METAL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/fsr4_metal/*.metal
             ${CMAKE_CURRENT_SOURCE_DIR}/fsr4_metal/*.h ${CMAKE_CURRENT_SOURCE_DIR}/fsr4_metal/*.py)
        set(metal_env)
        if (BB_METAL_DEVELOPER_DIR)
            set(metal_env DEVELOPER_DIR=${BB_METAL_DEVELOPER_DIR})
        endif()
        set(bb_fsr4_args --src ${BB_FSR4_NATIVE_SRC} --out ${BB_FSR4_METAL_OUT}
                        --profiles ${BB_FSR411_PROFILES})
        if (BB_FSR412_MODEL)
            list(APPEND bb_fsr4_args --fsr412-model ${BB_FSR412_MODEL})
        endif()
        if (BB_FSR411_LOSSY2_WEIGHTS)
            list(APPEND bb_fsr4_args --lossy2-weights ${BB_FSR411_LOSSY2_WEIGHTS})
        endif()
        add_custom_command(OUTPUT ${BB_FSR4_METAL_OUT}/.built
            COMMAND ${CMAKE_COMMAND} -E env ${metal_env} $<TARGET_FILE:Python3::Interpreter>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fsr4_metal/build_assets.py ${bb_fsr4_args}
            COMMAND ${CMAKE_COMMAND} -E touch ${BB_FSR4_METAL_OUT}/.built
            DEPENDS ${BB_FSR4_METAL_SOURCES} VERBATIM)
        add_custom_target(fsr4_metal_assets DEPENDS ${BB_FSR4_METAL_OUT}/.built)
        add_dependencies(bbgpu fsr4_metal_assets)
    else()
        message(WARNING "fsr4-native-macos or DXC is not in deps (${BB_FSR4_NATIVE_SRC}): "
                        "FSR 4.1.x on Metal is not built (scripts/macos/build-deps.sh)")
    endif()
endif()
if (APPLE)
    # MetalFX upscaler's Metal side (shim/bbport_metalfx.mm). MetalFX.framework is dlopen'ed
    # at run time, so nothing links against it; ARC is required by the ObjC objects.
    enable_language(OBJCXX)
    set_source_files_properties(shim/bbport_metalfx.mm PROPERTIES LANGUAGE OBJCXX
        COMPILE_OPTIONS "-fobjc-arc")
    target_sources(bbgpu PRIVATE shim/bbport_metalfx.mm)
    target_link_libraries(bbgpu PUBLIC "-framework QuartzCore" "-framework Metal" "-framework Cocoa")
    # Zydis's package names Zycore by -lZycore only: the deps prefix's lib directory.
    target_link_directories(bbgpu PRIVATE ${CMAKE_PREFIX_PATH}/lib)
else()
    # Loaded with bb-probe at startup (DT_NEEDED, never dlopen): thread_local access without a
    # TLS descriptor call (_dl_tlsdesc_return was ~2% of the draw recording thread).
    target_compile_options(bbgpu PRIVATE -ftls-model=initial-exec)
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
# The Metal backend (R5 N5, docs/NATIVE_RENDERER.md): SPIR-V to MSL through SPIRV-Cross, Metal
# pipeline states built from the shared pipeline keys. Adapted from Wooloomooloo2/
# bloodborne_mac_port (0b50589) and Huax-1839/bloodborne-macos (GPL-2.0-or-later). MoltenVK stays
# the renderer; BB_METAL_SHADOW=1 only builds a Metal pipeline beside each Vulkan one.
if (APPLE)
    enable_language(OBJCXX)
    find_package(spirv_cross_core CONFIG REQUIRED)
    find_package(spirv_cross_glsl CONFIG REQUIRED)
    find_package(spirv_cross_msl CONFIG REQUIRED)
    file(GLOB METAL_SOURCES CONFIGURE_DEPENDS shadps4/video_core/renderer_metal/*.cpp
         shadps4/video_core/renderer_metal/*.mm)
    target_sources(bbgpu PRIVATE ${METAL_SOURCES})
    # The settings overlay on the Metal frame: Dear ImGui's Metal backend (private, like the rest
    # of Dear ImGui) and its use (shim/bbport_overlay_metal.mm).
    target_sources(bbgpu PRIVATE third_party/imgui/backends/imgui_impl_metal.mm
        shim/bbport_overlay_metal.mm)
    set_source_files_properties(third_party/imgui/backends/imgui_impl_metal.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc;-fvisibility=hidden;-fvisibility-inlines-hidden")
    set_source_files_properties(shim/bbport_overlay_metal.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    foreach(source IN LISTS METAL_SOURCES)
        if (source MATCHES "\\.mm$")
            set_source_files_properties(${source} PROPERTIES COMPILE_OPTIONS
                "-fobjc-arc;-UVK_USE_PLATFORM_METAL_EXT;-Wno-unguarded-availability-new")
        endif()
    endforeach()
    target_link_libraries(bbgpu PRIVATE spirv-cross-msl spirv-cross-glsl spirv-cross-core
        "-framework Metal" "-framework QuartzCore" "-framework Foundation")
    target_compile_definitions(bbgpu PRIVATE BB_METAL_BACKEND=1)
endif()
if (BB_FSR)
    target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
endif()
target_compile_definitions(bbgpu PRIVATE BB_FSR=$<BOOL:${BB_FSR}>)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
if (NOT APPLE)
    target_link_libraries(bbgpu PUBLIC PkgConfig::X11)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
find_package(Threads REQUIRED)
add_executable(image-upload-hash-test EXCLUDE_FROM_ALL ../tests/test_image_upload_hash.cpp)
target_compile_options(image-upload-hash-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(image-upload-hash-test PRIVATE shadps4)
target_link_libraries(image-upload-hash-test PRIVATE PkgConfig::XXHASH)
add_executable(slot-vector-test EXCLUDE_FROM_ALL ../tests/test_slot_vector.cpp
    shim/common/slot_vector_pages.cpp)
target_compile_options(slot-vector-test PRIVATE -UNDEBUG -D_DEBUG -Wall -Wextra -Werror)
target_include_directories(slot-vector-test PRIVATE shim shadps4)
target_link_libraries(slot-vector-test PRIVATE fmt::fmt)
add_executable(handle-table-test EXCLUDE_FROM_ALL ../tests/test_handle_table.cpp)
target_compile_options(handle-table-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(handle-table-test PRIVATE shadps4)
add_executable(avplayer-queue-test EXCLUDE_FROM_ALL ../tests/test_avplayer_queue.cpp)
target_compile_options(avplayer-queue-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(avplayer-queue-test PRIVATE shadps4)
target_link_libraries(avplayer-queue-test PRIVATE Threads::Threads)
add_executable(avplayer-clock-test EXCLUDE_FROM_ALL ../tests/test_avplayer_clock.cpp)
target_compile_options(avplayer-clock-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(avplayer-clock-test PRIVATE shadps4)
target_link_libraries(avplayer-clock-test PRIVATE Threads::Threads)
add_executable(fixed-quad-tessellation-test EXCLUDE_FROM_ALL ../tests/test_fixed_quad_tessellation.cpp)
target_compile_options(fixed-quad-tessellation-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(fixed-quad-tessellation-test PRIVATE shadps4)
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
add_executable(cp-decoder-test EXCLUDE_FROM_ALL ../tests/test_cp_decoder.cpp cp/cp_decoder.cpp)
target_compile_options(cp-decoder-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(cp-decoder-test PRIVATE cp)
add_executable(settings-test EXCLUDE_FROM_ALL ../tests/test_settings.cpp shim/bbport_settings.cpp)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE .. shim)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

add_executable(translation-memory-test EXCLUDE_FROM_ALL ../tests/test_translation_memory.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(translation-memory-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(translation-memory-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_translation_memory.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(translation-memory-test PRIVATE bbgpu)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(trophy-queue-test EXCLUDE_FROM_ALL ../tests/test_trophy_queue.cpp)
target_include_directories(trophy-queue-test PRIVATE shim)
target_compile_options(trophy-queue-test PRIVATE -UNDEBUG)

add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

# What ctest runs, and what bash build.sh --test builds: neither launches the game nor needs a
# Vulkan device.
#
# Not here, because they stopped compiling when the code moved under them (they were not built by
# anything before): motion-shader-test (missing symbols at link time), scene-resolution-test
# (SceneTargets takes other arguments), taa-shader-test and camera-motion-test (UniqueImage is
# Vulkan::UniqueImage now), translation-memory-test (BufferCopyShaderHash is gone). Their targets
# stay, so a repair has somewhere to start.
if (BUILD_TESTING)
    set(BBGPU_TESTS image-upload-hash-test slot-vector-test handle-table-test avplayer-queue-test
        avplayer-clock-test fixed-quad-tessellation-test motion-history-test ui-composition-test
        cp-decoder-test settings-test trophy-queue-test upscaler-support-test)
    foreach(test IN LISTS BBGPU_TESTS)
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
    add_custom_target(bbgpu-tests DEPENDS ${BBGPU_TESTS})
endif()

# The FSR 4 benchmark (tools/fsr4_verify.sh, tools/fsr4cap/): the Vulkan path's provider, so Linux
# only — it links the Vulkan entry points, and macOS has no loader to give it.
if (BB_FSR AND NOT APPLE)
    add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
        shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
    set_target_properties(fsr4-bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${BB_BENCH_OUT})
    target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
    target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan
        ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)
endif()

# The online module: on macOS libbbnet20 (gpu/bbnet20: shadPS4 0.20's network code and The
# Hunter's Dream backend; its libraries are in deps/), elsewhere libbbnet (gpu/bbnet: shadp2p's,
# from shell.nix). BB_NET=OFF leaves it out; without it the port plays offline.
option(BB_NET "Build the online module" ON)
if (BB_NET AND APPLE)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(OpenSSL REQUIRED)
    find_package(ZLIB REQUIRED)
    find_path(MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc REQUIRED)
    find_library(MINIUPNPC_LIBRARY NAMES libminiupnpc.a miniupnpc REQUIRED)
    add_subdirectory(bbnet20)
    message(STATUS "Online module: on (libbbnet20.dylib)")
elseif (BB_NET)
    find_package(Protobuf CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(OpenSSL QUIET)
    find_package(ZLIB QUIET)
    find_path(MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc)
    find_library(MINIUPNPC_LIBRARY NAMES miniupnpc)
    if (Protobuf_FOUND AND nlohmann_json_FOUND AND OpenSSL_FOUND AND ZLIB_FOUND AND
        MINIUPNPC_INCLUDE AND MINIUPNPC_LIBRARY)
        add_subdirectory(bbnet)
        message(STATUS "Online module: on (libbbnet.so)")
    else()
        message(STATUS "Online module: off (protobuf, nlohmann_json, OpenSSL, zlib or miniupnpc missing)")
    endif()
endif()
