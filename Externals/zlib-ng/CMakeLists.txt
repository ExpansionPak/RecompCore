set(ZLIB_ENABLE_TESTS OFF)
set(ZLIB_COMPAT ON)
set(SKIP_INSTALL_ALL ON)

option(BUILD_SHARED_LIBS "Build shared library" OFF)

add_subdirectory(zlib-ng)

# Set ZLIB variables for find_package used by other projects
set(ZLIB_INCLUDE_DIR ${CMAKE_BINARY_DIR}/zlib-ng CACHE STRING "Path to zlib include directory")
set(ZLIB_LIBRARY ZLIB::ZLIB CACHE STRING "Path to zlib library")

# Setup zlib alias project so FindZLIB doesn't recreate it
if(NOT TARGET ZLIB::ZLIB)
    if(TARGET zlib-ng)
        add_library(ZLIB::ZLIB ALIAS zlib-ng)
    elseif(TARGET zlib)
        add_library(ZLIB::ZLIB ALIAS zlib)
    endif()
endif()
# dolphin_disable_warnings(zlib)