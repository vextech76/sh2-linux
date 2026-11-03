cmake_minimum_required(VERSION 3.25)
project(lantern VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LANTERN_BUILD_TESTS "Build unit and integration tests" ON)
option(LANTERN_WERROR "Treat warnings as errors" OFF)

# Never allow building inside supplied game data or the source tree root.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed. Use: cmake --preset debug")
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(Vulkan REQUIRED)
find_package(tomlplusplus REQUIRED CONFIG)
find_program(GLSLC glslc REQUIRED)

add_library(lantern_warnings INTERFACE)
target_compile_options(lantern_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
  $<$<BOOL:${LANTERN_WERROR}>:-Werror>)

add_subdirectory(src)

if(LANTERN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
