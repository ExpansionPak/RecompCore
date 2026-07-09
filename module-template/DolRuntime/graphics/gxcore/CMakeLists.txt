add_library(dolruntime_gxcore STATIC
    src/gxcore.cpp
    src/texture_decode.cpp
)
add_library(DolRuntime::gxcore ALIAS dolruntime_gxcore)

target_compile_features(dolruntime_gxcore PUBLIC cxx_std_20)
target_include_directories(dolruntime_gxcore
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(dolruntime_gxcore
    PUBLIC
        DolRuntime::aurora_render_sink
)

if(BUILD_TESTING)
    add_executable(gxcore_tests
        tests/gxcore_wgsl_test.cpp
    )
    target_link_libraries(gxcore_tests PRIVATE DolRuntime::gxcore)
    add_test(NAME gxcore_tests COMMAND gxcore_tests)

    add_executable(gxcore_texture_decode_tests
        tests/gxcore_texture_decode_test.cpp
    )
    target_link_libraries(gxcore_texture_decode_tests PRIVATE DolRuntime::gxcore)
    add_test(NAME gxcore_texture_decode_tests COMMAND gxcore_texture_decode_tests)
endif()
