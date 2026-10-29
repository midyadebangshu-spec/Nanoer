cmake_minimum_required(VERSION 3.20)
project(nanoer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NANOER_BUILD_TESTS "Build unit tests" ON)
option(NANOER_BUILD_FUSE "Build the FUSE mount program nanoerfs (needs libfuse 3.12+)" ON)
option(NANOER_BUILD_FUZZERS "Build libFuzzer targets (needs Clang)" OFF)
option(NANOER_WERROR "Treat compiler warnings as errors" OFF)
set(NANOER_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

if(NANOER_SANITIZE)
  add_compile_options(-fsanitize=${NANOER_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${NANOER_SANITIZE})
endif()

if(NANOER_BUILD_FUZZERS)
  # Coverage instrumentation for all of Nanoer's code, so the fuzzers can
  # see which paths an input reaches (the targets add the fuzzer runtime).
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd>=1.4.5)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4>=1.9.0)
pkg_check_modules(LZMA REQUIRED IMPORTED_TARGET liblzma>=5.2.0)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash>=0.8.0)
if(NANOER_BUILD_FUSE)
  pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.12)
endif()

add_subdirectory(third_party/blake3)

# Warnings for Nanoer's own code only (not vendored libraries).
add_library(nanoer_warnings INTERFACE)
target_compile_options(nanoer_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align
  -Wold-style-cast -Wimplicit-fallthrough
  # Designated initializers deliberately leave defaulted fields out.
  -Wno-missing-field-initializers
  $<$<BOOL:${NANOER_WERROR}>:-Werror>)

add_subdirectory(src)
add_subdirectory(bench)

if(NANOER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(NANOER_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
