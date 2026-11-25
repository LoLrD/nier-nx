cmake_minimum_required(VERSION 3.25)
project(nier LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

add_compile_definitions(REX_PPC_FIXED_FLUSH_MODE REX_STORE_CARDS REX_PLAIN_GUEST_MEMORY)

include(generated/rexglue.cmake)

set(NIER_SOURCES
    src/main.cpp
    src/nier_d3d_wait.cpp
    src/nier_audio_mix.cpp
    src/nier_audio_mix_hook.cpp
    src/nier_frame_pacing.cpp
    src/nier_frame_pacing_hook.cpp
    src/nier_npc_lod.cpp
    src/nier_npc_lod_hook.cpp
    src/nier_input_driver.cpp
    src/nier_input_script.cpp
    src/nier_resolution.cpp
    src/nier_switch_prompts.cpp
    src/nier_archive_patches.cpp
    src/nier_language.cpp
    src/language_patches.cpp
    src/cpk_toc.cpp
    src/read_patches.cpp
    src/native/arena_ranges.cpp
    src/native/draw_constants.cpp
    src/native/draw_state_filter.cpp
    src/native/gpu_progress.cpp
    src/native/gpu_registers.cpp
    src/native/gpu_times.cpp
    src/native/guest_mirror.cpp
    src/native/mirror_sync.cpp
    src/native/native_graphics_system.cpp
    src/native/native_renderer.cpp
    src/native/native_shaders.cpp
    src/native/perf_modes.cpp
    src/native/phased_hash.cpp
    src/native/pipeline_report.cpp
    src/native/pipeline_state.cpp
    src/native/pipelines.cpp
    src/native/pm4_executor.cpp
    src/native/primitive_setup.cpp
    src/native/render_targets.cpp
    src/native/resolve_plan.cpp
    src/native/resolves.cpp
    src/native/scene_upscale.cpp
    src/native/shader_cache.cpp
    src/native/shader_dump.cpp
    src/native/slow_frames.cpp
    src/native/store_cards.cpp
    src/native/spirv_specialize.cpp
    src/native/submissions.cpp
    src/native/target_keys.cpp
    src/native/texture_loader.cpp
    src/native/textures.cpp
    src/native/vk_recorder.cpp
)
add_executable(nier ${NIER_SOURCES})
file(GLOB NIER_TRANSLATOR_SOURCES ${REXSDK_DIR}/src/graphics/pipeline/shader/*.cpp
     ${REXSDK_DIR}/include/rex/graphics/pipeline/shader/*.h)
list(SORT NIER_TRANSLATOR_SOURCES)
set(NIER_TRANSLATOR_HASHES "")
foreach(translator_source ${NIER_TRANSLATOR_SOURCES})
    file(SHA256 ${translator_source} translator_hash)
    string(APPEND NIER_TRANSLATOR_HASHES ${translator_hash})
endforeach()
string(SHA256 NIER_TRANSLATOR_HASH "${NIER_TRANSLATOR_HASHES}")
string(SUBSTRING ${NIER_TRANSLATOR_HASH} 0 16 NIER_TRANSLATOR_FINGERPRINT)
target_compile_definitions(nier PRIVATE NIER_TRANSLATOR_FINGERPRINT=0x${NIER_TRANSLATOR_FINGERPRINT}ull)
target_include_directories(nier PRIVATE src ${REXSDK_DIR}/thirdparty/glslang ${REXSDK_DIR}/thirdparty/xxHash
                           ${REXSDK_DIR}/src/ui/shaders/vulkan_spirv)

rexglue_setup_target(nier GPU_PLUGINS xenos)
target_compile_options(nier_recomp PRIVATE -fno-strict-aliasing)
set(NIER_INDIRECT_DISPATCH 2 CACHE STRING "Guest indirect calls: 0 legacy table, 1 compact table, 2 inline cache per call site")
option(NIER_INDIRECT_DISPATCH_VERIFY "Compare guest indirect call targets with the legacy lookup" OFF)
target_compile_definitions(nier_recomp PRIVATE REX_INDIRECT_DISPATCH=${NIER_INDIRECT_DISPATCH})
if(NIER_INDIRECT_DISPATCH_VERIFY)
    target_compile_definitions(nier_recomp PRIVATE REX_INDIRECT_DISPATCH_VERIFY=1)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH)
    if(TARGET rexgpu-xenos)
        target_link_libraries(nier PRIVATE rexgpu-xenos)
    else()
        target_link_libraries(nier PRIVATE rex::gpu-xenos)
    endif()
endif()

option(NIER_LTO "Link-time optimization of the game code and the port on the Switch" ON)
if(REXGLUE_PLATFORM_SWITCH AND NIER_LTO)
    target_compile_options(nier_recomp PRIVATE -flto=auto -fno-fat-lto-objects)
    target_compile_options(nier PRIVATE -flto=auto -fno-fat-lto-objects)
    target_link_options(nier PRIVATE -flto=auto -flto-partition=balanced -O3)
endif()

set(NIER_PGO "" CACHE STRING "Profile-guided optimization on the Switch: empty, generate (training build) or use")
set(NIER_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../out/pgo" CACHE PATH "Profile a NIER_PGO=use build reads")
if(REXGLUE_PLATFORM_SWITCH AND NIER_PGO STREQUAL "generate")
    foreach(pgo_target nier_recomp nier)
        target_compile_options(${pgo_target} PRIVATE -fprofile-generate=sdmc:/switch/nier/pgo
                               -fno-profile-values -fprofile-prefix-path=${CMAKE_BINARY_DIR})
    endforeach()
    target_compile_definitions(nier PRIVATE NIER_PGO_GENERATE)
    target_link_options(nier PRIVATE -fprofile-generate)
elseif(REXGLUE_PLATFORM_SWITCH AND NIER_PGO STREQUAL "use")
    foreach(pgo_target nier_recomp nier)
        target_compile_options(${pgo_target} PRIVATE -fprofile-use=${NIER_PGO_DIR} -fprofile-partial-training
                               -fprofile-prefix-path=${CMAKE_BINARY_DIR} -Wno-missing-profile)
    endforeach()
    target_link_options(nier PRIVATE -fprofile-use=${NIER_PGO_DIR} -fprofile-partial-training)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    rexglue_switch_add_nro(nier
        NAME "NieR"
        AUTHOR "nier-nx"
        VERSION "0.1.0")
endif()
