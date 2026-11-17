cmake_minimum_required(VERSION 3.25)
project(nier LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

include(generated/rexglue.cmake)

set(NIER_SOURCES
    src/main.cpp
    src/nier_input_driver.cpp
    src/nier_input_script.cpp
    src/native/gpu_registers.cpp
    src/native/native_graphics_system.cpp
    src/native/native_shaders.cpp
    src/native/pm4_executor.cpp
    src/native/shader_dump.cpp
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
