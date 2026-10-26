add_library(nanoer_core STATIC
  core/cache.cpp
  core/check.cpp
  core/checksum.cpp
  core/codec.cpp
  core/dedup.cpp
  core/encoding.cpp
  core/error.cpp
  core/format.cpp
  core/fs.cpp
  core/gc.cpp
  core/io.cpp
  core/journal.cpp
  core/metadata.cpp
  core/mkfs.cpp
  core/record.cpp
  core/segment.cpp
  core/stats.cpp
  core/thread_pool.cpp
)
target_include_directories(nanoer_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nanoer_core
  PUBLIC PkgConfig::ZSTD PkgConfig::LZ4 PkgConfig::LZMA PkgConfig::XXHASH blake3
  PRIVATE nanoer_warnings)
set_target_properties(nanoer_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(mkfs.nanoer tools/mkfs_nanoer.cpp tools/common.cpp)
target_link_libraries(mkfs.nanoer PRIVATE nanoer_core nanoer_warnings)

add_executable(nanoer tools/nanoer.cpp tools/common.cpp)
target_link_libraries(nanoer PRIVATE nanoer_core nanoer_warnings)

add_executable(nanoer-fsx tools/nanoer_fsx.cpp)
target_link_libraries(nanoer-fsx PRIVATE nanoer_warnings)

install(TARGETS mkfs.nanoer nanoer RUNTIME DESTINATION sbin)
install(TARGETS nanoer-fsx RUNTIME DESTINATION bin)

if(NANOER_BUILD_FUSE)
  add_executable(nanoerfs fuse/nanoerfs.cpp)
  target_link_libraries(nanoerfs PRIVATE nanoer_core nanoer_warnings PkgConfig::FUSE3)
  # mount(8) runs /sbin/mount.TYPE for `mount -t TYPE`; it is the same program.
  add_custom_command(TARGET nanoerfs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink nanoerfs mount.nanoer
    WORKING_DIRECTORY $<TARGET_FILE_DIR:nanoerfs>)
  install(TARGETS nanoerfs RUNTIME DESTINATION bin)
  install(CODE "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/sbin\")
    file(CREATE_LINK ../bin/nanoerfs \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/sbin/mount.nanoer\" SYMBOLIC)")
endif()
target_compile_definitions(nanoer PRIVATE NANOER_VERSION="${PROJECT_VERSION}")
# fsck(8) runs fsck.TYPE for `fsck -t TYPE` and boot-time checks.
add_custom_command(TARGET nanoer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink nanoer fsck.nanoer
  WORKING_DIRECTORY $<TARGET_FILE_DIR:nanoer>)
install(CODE "file(CREATE_LINK nanoer \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/sbin/fsck.nanoer\" SYMBOLIC)")
