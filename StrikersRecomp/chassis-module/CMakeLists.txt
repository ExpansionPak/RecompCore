cmake_minimum_required(VERSION 3.16)
project(strikers_chassis_module C)

# Strikers (G4QE01) native module for dolphin-chassis: generated chunks +
# DolRecomp CPU runtime + ABI export glue, no other host dependencies.

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../generated")
set(DOLRUNTIME_DIR "${REPO_ROOT}/DolRuntime" CACHE PATH "DolRuntime checkout")
set(CHASSIS_ABI_DIR "${REPO_ROOT}/dolphin-chassis/Source/Core/Core/PowerPC/StaticRecomp"
    CACHE PATH "dolphin-chassis StaticRecomp ABI directory")

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

add_library(gG4QE01_recomp SHARED
  module_export.c
  "${MODULE_TABLES}"
  "${DOLRUNTIME_DIR}/src/core/cpu.c"
  ${CHUNK_SOURCES})

set_target_properties(gG4QE01_recomp PROPERTIES PREFIX "" OUTPUT_NAME "gG4QE01_recomp")
set_source_files_properties("${MODULE_TABLES}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

target_include_directories(gG4QE01_recomp PRIVATE
  "${GENERATED_DIR}"
  "${DOLRUNTIME_DIR}/include"
  "${CHASSIS_ABI_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(gG4QE01_recomp PRIVATE m)

# Dispatch performance: ThinLTO so the DolRuntime cpu.c helpers
# (mem_read32/translate_addr/read_be32, the FP & paired-single ops, ppc_host_call)
# inline into the generated chunk call sites. Profiling showed the module's
# dominant per-op cost was out-of-line helper CALLS across translation units
# (mem_read32 alone ~10% of the CPU thread in a heavy match). Semantics are
# unchanged — no -ffast-math, so FP stays bit-exact — and guarded by the oracle
# (239/0) + the lockstep differential (0 divergences).
target_compile_options(gG4QE01_recomp PRIVATE -flto=thin)
target_link_options(gG4QE01_recomp PRIVATE -flto=thin)
