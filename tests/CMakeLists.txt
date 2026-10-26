find_package(GTest 1.10 REQUIRED)
include(GoogleTest)

add_executable(nanoer_unit_tests
  unit/cache_pool_test.cpp
  unit/check_test.cpp
  unit/checksum_test.cpp
  unit/crash_test.cpp
  unit/dedup_test.cpp
  unit/damage_test.cpp
  unit/fs_model_test.cpp
  unit/gc_test.cpp
  unit/fs_test.cpp
  unit/codec_test.cpp
  unit/format_test.cpp
  unit/io_test.cpp
  unit/journal_test.cpp
  unit/metadata_test.cpp
  unit/mkfs_test.cpp
  unit/pack_test.cpp
  unit/record_test.cpp
  unit/segment_test.cpp
  unit/stats_test.cpp
)
target_link_libraries(nanoer_unit_tests PRIVATE nanoer_core nanoer_warnings GTest::gtest_main)
gtest_discover_tests(nanoer_unit_tests DISCOVERY_TIMEOUT 60)

# The command-line tools, run end to end.
add_test(NAME tools_smoke
  COMMAND ${CMAKE_COMMAND}
    -DMKFS=$<TARGET_FILE:mkfs.nanoer> -DNANOER=$<TARGET_FILE:nanoer>
    -DFSCK=$<TARGET_FILE_DIR:nanoer>/fsck.nanoer
    -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/tools_smoke
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools_smoke.cmake)

# Integration tests on a real FUSE mount (skipped where FUSE is unavailable).
if(NANOER_BUILD_FUSE)
  add_executable(nanoer_posix_tests integration/posix_test.cpp)
  target_link_libraries(nanoer_posix_tests PRIVATE nanoer_warnings GTest::gtest_main)

  set(RUN_MOUNTED ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_mounted.sh)
  set(TOOLS_DIR $<TARGET_FILE_DIR:nanoerfs>)
  add_test(NAME integration_posix COMMAND ${RUN_MOUNTED} ${TOOLS_DIR} posix $<TARGET_FILE:nanoer_posix_tests>)
  add_test(NAME integration_fsx COMMAND ${RUN_MOUNTED} ${TOOLS_DIR} fsx)
  # A tree nothing else writes to while the test runs (not the source tree,
  # where builds may be running).
  add_test(NAME integration_persist COMMAND ${RUN_MOUNTED} ${TOOLS_DIR} persist /usr/include)
  add_test(NAME integration_kill COMMAND ${RUN_MOUNTED} ${TOOLS_DIR} kill)
  # The same on small stores with small segments: garbage collection runs
  # all the time.
  add_test(NAME integration_fsx_small COMMAND ${RUN_MOUNTED} ${TOOLS_DIR} fsx)
  add_test(NAME integration_kill_small COMMAND ${RUN_MOUNTED} ${TOOLS_DIR} kill)
  set_tests_properties(integration_fsx_small PROPERTIES ENVIRONMENT
    "FSX_OPS=20000;NANOER_MKFS_OPTS=--size 40M --segment-size 1M")
  set_tests_properties(integration_kill_small PROPERTIES ENVIRONMENT
    "KILL_LOAD=fsx;KILL_LIVE_LIMIT_MB=40;NANOER_MKFS_OPTS=--size 160M --segment-size 1M")
  set_tests_properties(integration_posix integration_fsx integration_persist integration_kill
                       integration_fsx_small integration_kill_small PROPERTIES
    SKIP_RETURN_CODE 77 LABELS integration RUN_SERIAL TRUE TIMEOUT 1200)
  set_tests_properties(integration_fsx PROPERTIES ENVIRONMENT "FSX_OPS=10000")
endif()
