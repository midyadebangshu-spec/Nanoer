find_package(GTest 1.10 REQUIRED)
include(GoogleTest)

add_executable(nanoer_unit_tests
  unit/checksum_test.cpp
  unit/codec_test.cpp
  unit/format_test.cpp
  unit/io_test.cpp
  unit/mkfs_test.cpp
  unit/record_test.cpp
  unit/segment_test.cpp
)
target_link_libraries(nanoer_unit_tests PRIVATE nanoer_core nanoer_warnings GTest::gtest_main)
gtest_discover_tests(nanoer_unit_tests DISCOVERY_TIMEOUT 60)

# The command-line tools, run end to end.
add_test(NAME tools_smoke
  COMMAND ${CMAKE_COMMAND}
    -DMKFS=$<TARGET_FILE:mkfs.nanoer> -DNANOER=$<TARGET_FILE:nanoer>
    -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/tools_smoke
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools_smoke.cmake)
