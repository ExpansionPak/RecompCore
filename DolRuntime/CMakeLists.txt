cmake_minimum_required(VERSION 3.16)
project(DolRuntime LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(CTest)

add_library(dolruntime STATIC
    src/core/cpu.c
    src/loader.c
    src/boot.c
    src/aram.c
    src/dvd.c
    src/event_clock.c
    src/guest_memory.c
    src/gx_recomp.c
    src/interrupts.c
    src/si.c
    src/exi.c
    src/di.c
    src/mmio_bus.c
    src/platform.c
    src/audio_dma.c
    src/vi_clock.c
    src/memory_card.c
    src/headless_backend.c
    src/savestate.c
)
add_library(DolRuntime::runtime ALIAS dolruntime)

target_include_directories(dolruntime
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

include(CheckLibraryExists)
check_library_exists(m sqrt "" DOLRUNTIME_HAVE_LIBM)
if(DOLRUNTIME_HAVE_LIBM)
    target_link_libraries(dolruntime PUBLIC m)
endif()

option(DOLRUNTIME_ENABLE_AURORA
       "Build the Aurora graphics/input/audio backend" ON)
option(DOLRUNTIME_ENABLE_AURORA_RECOMP
       "Build DolRuntime-owned Aurora recomp-mode frontend/sink modules" ON)
set(DOLRUNTIME_AURORA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/graphics/aurora"
    CACHE PATH "Path to the DolRuntime-owned Aurora fork (graphics/aurora)")

if(DOLRUNTIME_ENABLE_AURORA_RECOMP)
    enable_language(CXX)
    add_subdirectory(graphics/frontend)
    add_subdirectory(graphics/gxcore)
endif()

if(DOLRUNTIME_ENABLE_AURORA)
    if(NOT EXISTS "${DOLRUNTIME_AURORA_DIR}/include/aurora/aurora.h")
        message(FATAL_ERROR
            "Aurora not found at ${DOLRUNTIME_AURORA_DIR}. Set "
            "-DDOLRUNTIME_AURORA_DIR=<path> or disable "
            "DOLRUNTIME_ENABLE_AURORA.")
    endif()

    enable_language(CXX)
    set(AURORA_RECOMP_TEST_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/graphics/frontend/tests/recomp_fifo_test.cpp")
    add_subdirectory("${DOLRUNTIME_AURORA_DIR}"
                     "${CMAKE_BINARY_DIR}/aurora" EXCLUDE_FROM_ALL)

    add_library(dolruntime_aurora STATIC
        backends/aurora/aurora_backend.cpp
    )
    add_library(DolRuntime::aurora ALIAS dolruntime_aurora)
    target_include_directories(dolruntime_aurora
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:include>"
        PRIVATE
            "${DOLRUNTIME_AURORA_DIR}/lib"
    )
    target_compile_features(dolruntime_aurora PUBLIC cxx_std_20)
    target_link_libraries(dolruntime_aurora
        PUBLIC
            dolruntime
            aurora::core aurora::gx aurora::vi aurora::os
            aurora::pad aurora::si aurora::mtx aurora::main
            absl::flat_hash_map
    )
    if(DOLRUNTIME_ENABLE_AURORA_RECOMP)
        target_compile_definitions(dolruntime_aurora
            PRIVATE DOLRUNTIME_HAS_AURORA_RECOMP=1)
        target_link_libraries(dolruntime_aurora
            PUBLIC DolRuntime::retail_gx_frontend DolRuntime::trace_io)
        # 63/Mfin live cutover: the backend drives a GxCoreSink directly (bypasses
        # live Aurora gx under DOL_GX_CORE), so it needs the gxcore headers and
        # symbols — not just the transitive link through aurora::gx.
        target_link_libraries(dolruntime_aurora PRIVATE DolRuntime::gxcore)
        # pixel replay (Mode B) — the same dolgx_replay tool gains
        # --pixels when built in an Aurora-enabled tree (the StrikersRecomp
        # GUI build); build-headless keeps the digest-only tool.
        # --core (Mode B2) routes draws through the gxcore modules
        # instead of the live Aurora gx layer; it reaches into the fork's
        # lib/ for the gxcore_draw submission seam.
        target_sources(dolgx_replay PRIVATE
            graphics/frontend/tools/dolgx_replay_pixels.cpp
            graphics/frontend/tools/dolgx_replay_core.cpp)
        target_compile_definitions(dolgx_replay
            PRIVATE DOLGX_REPLAY_HAS_PIXELS=1 DOLGX_REPLAY_HAS_CORE=1)
        target_include_directories(dolgx_replay
            PRIVATE "${DOLRUNTIME_AURORA_DIR}/lib")
        target_link_libraries(dolgx_replay
            PRIVATE DolRuntime::aurora DolRuntime::gxcore)
    else()
        target_compile_definitions(dolruntime_aurora
            PRIVATE DOLRUNTIME_HAS_AURORA_RECOMP=0)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(dolruntime_tests
        tests/runtime_tests.c
    )
    target_link_libraries(dolruntime_tests PRIVATE DolRuntime::runtime)
    add_test(NAME dolruntime_tests COMMAND dolruntime_tests)
endif()
