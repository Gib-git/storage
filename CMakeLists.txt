cmake_minimum_required(VERSION 3.21)

project(Storage
    VERSION 0.6.0
    DESCRIPTION "Index, deduplicate, organize and clean your disks and NAS"
    HOMEPAGE_URL "https://github.com/Gib-git/storage"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(STORAGE_BUILD_GUI   "Build the Qt desktop application" ON)
option(STORAGE_BUILD_TESTS "Build the test suite" ON)

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(cmake/Deps.cmake)

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(STORAGE_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

if(STORAGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
