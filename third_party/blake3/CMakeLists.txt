# Official BLAKE3 C implementation, built with runtime SIMD dispatch.
add_library(blake3 STATIC blake3.c blake3_dispatch.c blake3_portable.c)
target_include_directories(blake3 SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(blake3 PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  target_sources(blake3 PRIVATE blake3_sse2.c blake3_sse41.c blake3_avx2.c blake3_avx512.c)
  set_source_files_properties(blake3_sse2.c PROPERTIES COMPILE_OPTIONS "-msse2")
  set_source_files_properties(blake3_sse41.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
  set_source_files_properties(blake3_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2")
  set_source_files_properties(blake3_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  target_sources(blake3 PRIVATE blake3_neon.c)
  target_compile_definitions(blake3 PRIVATE BLAKE3_USE_NEON=1)
else()
  target_compile_definitions(blake3 PRIVATE
    BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)
endif()
