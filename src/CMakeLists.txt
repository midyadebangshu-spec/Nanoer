add_library(nanoer_core STATIC
  core/checksum.cpp
  core/codec.cpp
  core/error.cpp
  core/format.cpp
  core/io.cpp
  core/mkfs.cpp
  core/record.cpp
  core/segment.cpp
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

install(TARGETS mkfs.nanoer nanoer RUNTIME DESTINATION sbin)
target_compile_definitions(nanoer PRIVATE NANOER_VERSION="${PROJECT_VERSION}")
