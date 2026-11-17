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
)
add_executable(nier ${NIER_SOURCES})
target_include_directories(nier PRIVATE src)

rexglue_setup_target(nier GPU_PLUGINS xenos)

if(REXGLUE_PLATFORM_SWITCH)
    rexglue_switch_add_nro(nier
        NAME "NieR"
        AUTHOR "nier-nx"
        VERSION "0.1.0")
endif()
