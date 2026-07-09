cmake_minimum_required(VERSION 3.16)
project(recompcore_module C)

# Per-game native module for RecompCore: generated chunks + DolRecomp CPU
# runtime + ABI export glue, no other host dependencies.
#
# Configure with:
#   cmake -B build -DGAME_ID=G4QE01 \
#         -DGENERATED_DIR=/path/to/generated \
#         -DDOLRUNTIME_DIR=/path/to/DolRuntime

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GAME_ID "" CACHE STRING "Six-character disc ID, e.g. G4QE01")
if(NOT GAME_ID)
  message(FATAL_ERROR "Set -DGAME_ID=<disc id> (e.g. -DGAME_ID=G4QE01)")
endif()
set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "DolRecomp output directory (generated.h, generated_smc.txt, main.dol, chunks/)")
if(NOT EXISTS "${GENERATED_DIR}")
  message(FATAL_ERROR "Generated directory not found at ${GENERATED_DIR}. Set -DGENERATED_DIR=<path to DolRecomp output>")
endif()
set(DOLRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/DolRuntime" CACHE PATH "DolRuntime checkout")
if(NOT EXISTS "${DOLRUNTIME_DIR}")
  message(FATAL_ERROR "DolRuntime not found at ${DOLRUNTIME_DIR}. Set -DDOLRUNTIME_DIR=<path to DolRuntime>")
endif()
set(CHASSIS_ABI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Core/Core/PowerPC/StaticRecomp"
    CACHE PATH "RecompCore StaticRecomp ABI directory")

set(MODULE_NAME "g${GAME_ID}_recomp")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MODULE_TABLES "${CMAKE_CURRENT_BINARY_DIR}/module_tables.inc")
add_custom_command(
  OUTPUT "${MODULE_TABLES}"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol" "${MODULE_TABLES}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol"
  COMMENT "Generating module_tables.inc")

file(GLOB CHUNK_SOURCES "${GENERATED_DIR}/chunks/*.c")

add_library(${MODULE_NAME} SHARED
  module_export.c
  "${MODULE_TABLES}"
  "${DOLRUNTIME_DIR}/src/core/cpu.c"
  ${CHUNK_SOURCES})

option(ENABLE_UNITY_BUILD "Enable Unity Build" OFF)
set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${MODULE_NAME}" UNITY_BUILD ${ENABLE_UNITY_BUILD})
set_source_files_properties("${MODULE_TABLES}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}")

target_include_directories(${MODULE_NAME} PRIVATE
  "${GENERATED_DIR}"
  "${DOLRUNTIME_DIR}/include"
  "${CHASSIS_ABI_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(${MODULE_NAME} PRIVATE m)

option(ENABLE_LTO "Enable ThinLTO optimization" ON)

# ThinLTO so the DolRuntime cpu.c helpers inline into the generated chunk
# call sites; semantics unchanged (no -ffast-math, FP stays bit-exact).
if (ENABLE_LTO)
  target_compile_options(${MODULE_NAME} PRIVATE -flto=thin)
  target_link_options(${MODULE_NAME} PRIVATE -flto=thin)
endif()

target_compile_options(${MODULE_NAME} PRIVATE -fvisibility=hidden)
