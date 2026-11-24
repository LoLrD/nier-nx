cmake_minimum_required(VERSION 3.25)
project(nier LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

add_compile_definitions(REX_PPC_FIXED_FLUSH_MODE REX_STORE_CARDS)

include(generated/rexglue.cmake)

set(NIER_SOURCES
    src/main.cpp
    src/nier_d3d_wait.cpp
    src/nier_input_driver.cpp
    src/nier_input_script.cpp
    src/nier_resolution.cpp
    src/nier_switch_prompts.cpp
    src/nier_archive_patches.cpp
    src/nier_language.cpp
    src/language_patches.cpp
    src/cpk_toc.cpp
    src/read_patches.cpp
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
    src/native/shader_dump.cpp
    src/native/store_cards.cpp
    src/native/spirv_specialize.cpp
    src/native/submissions.cpp
    src/native/target_keys.cpp
    src/native/texture_loader.cpp
    src/native/textures.cpp
    src/native/vk_recorder.cpp
)
add_executable(nier ${NIER_SOURCES})
target_include_directories(nier PRIVATE src ${REXSDK_DIR}/thirdparty/glslang ${REXSDK_DIR}/thirdparty/xxHash)

rexglue_setup_target(nier GPU_PLUGINS xenos)
if(NOT REXGLUE_PLATFORM_SWITCH)
    if(TARGET rexgpu-xenos)
        target_link_libraries(nier PRIVATE rexgpu-xenos)
    else()
        target_link_libraries(nier PRIVATE rex::gpu-xenos)
    endif()
endif()

if(REXGLUE_PLATFORM_SWITCH)
    rexglue_switch_add_nro(nier
        NAME "NieR"
        AUTHOR "nier-nx"
        VERSION "0.1.0")
endif()
