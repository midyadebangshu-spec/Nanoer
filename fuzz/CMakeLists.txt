# libFuzzer targets. Build with the "fuzz" preset (Clang), then run e.g.
#   ./build/fuzz/fuzz/fuzz_decompress -max_total_time=60
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Fuzzers need Clang (use the fuzz preset)")
endif()

foreach(target fuzz_decompress fuzz_roundtrip fuzz_parse fuzz_image)
  add_executable(${target} ${target}.cpp)
  target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(${target} PRIVATE nanoer_core nanoer_warnings)
endforeach()
