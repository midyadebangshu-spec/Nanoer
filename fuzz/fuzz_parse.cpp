// Damaged or hostile on-disk structures: superblocks, segment headers,
// records, checkpoint snapshots, tree nodes and metadata operations must be
// rejected cleanly, never crash.
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "nanoer/encoding.h"
#include "nanoer/error.h"
#include "nanoer/format.h"
#include "nanoer/io.h"
#include "nanoer/legacy_metadata.h"
#include "nanoer/metadata.h"
#include "nanoer/record.h"
#include "nanoer/segment.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  nanoer::ByteView input(data, size);

  // Superblock: decode a 4 KiB block made from the input.
  nanoer::Bytes block(nanoer::kBlockSize, 0);
  std::copy_n(data, std::min<std::size_t>(size, block.size()), block.begin());
  nanoer::DecodedSuperblock sb = nanoer::decode_superblock(block);
  if (sb.state == nanoer::DecodedSuperblock::State::valid) {
    // Reserved bytes are ignored when read and written as zero, so an
    // accepted block need not re-encode to the same bytes. Its canonical
    // encoding must, though, decode to the same fields and be stable.
    nanoer::Bytes canonical = nanoer::encode_superblock(sb.sb);
    nanoer::DecodedSuperblock again = nanoer::decode_superblock(canonical);
    if (again.state != nanoer::DecodedSuperblock::State::valid ||
        nanoer::encode_superblock(again.sb) != canonical) {
      __builtin_trap();
    }
  }

  nanoer::Uuid uuid{};
  (void)nanoer::decode_segment_header(block, uuid, 0);

  // Records: scan the input as if it were part of a segment.
  nanoer::MemStore store(nanoer::Bytes(input.begin(), input.end()));
  std::uint64_t offset = 0;
  std::uint64_t seq = size >= 24 ? nanoer::load_le<std::uint64_t>(data + 16) : 0;
  while (auto record = nanoer::try_read_record(store, offset, store.size(), seq)) {
    if (record->header.type == nanoer::RecordType::data_chunk) {
      try {
        nanoer::Bytes chunk = nanoer::decode_data_chunk(*record);
        (void)chunk;
      } catch (const nanoer::Error& e) {
        if (e.code() != nanoer::Errc::corrupt) __builtin_trap();
      }
    }
    if (record->header.type == nanoer::RecordType::tree_node) {
      try {
        (void)nanoer::parse_tree_node(*record);
        (void)nanoer::BTree::decode(nanoer::decode_tree_node(*record));
      } catch (const nanoer::Error& e) {
        if (e.code() != nanoer::Errc::corrupt) __builtin_trap();
      }
    }
    offset += record->stored_size();
  }

  // A snapshot from before M6, with and without shared records. One that
  // is accepted must encode to a snapshot that decodes to the same state.
  for (bool shared : {false, true}) {
    try {
      nanoer::LegacyMetadata m = nanoer::LegacyMetadata::decode_checkpoint(input, nanoer::kDataStart, 20, shared);
      nanoer::Bytes again = m.encode_checkpoint();
      if (!(nanoer::LegacyMetadata::decode_checkpoint(again, nanoer::kDataStart, 20, shared) == m)) {
        __builtin_trap();
      }
      // Then the input, read as operations, applied to it.
      nanoer::Decoder ops(input);
      while (!ops.done()) m.apply(nanoer::decode_op(ops));
    } catch (const nanoer::Error& e) {
      if (e.code() != nanoer::Errc::corrupt) __builtin_trap();
    }
  }

  // A tree node.
  try {
    (void)nanoer::BTree::decode(input);
  } catch (const nanoer::Error& e) {
    if (e.code() != nanoer::Errc::corrupt) __builtin_trap();
  }

  // A tree checkpoint (its root is read from memory, so a valid one is
  // rare), and the input as operations applied to a small tree.
  nanoer::Metadata::Options options;
  options.tree = {.node_bytes = 512, .cache_bytes = 1 << 20};
  options.account_nodes = false;
  auto io = std::make_shared<nanoer::MemNodeIO>();
  try {
    nanoer::Metadata m = nanoer::Metadata::decode_checkpoint(input, nanoer::kDataStart, 20, options, io);
    (void)m.attrs(nanoer::kRootInode);
  } catch (const nanoer::Error& e) {
    if (e.code() != nanoer::Errc::corrupt) __builtin_trap();
  }
  try {
    nanoer::Metadata m = nanoer::Metadata::empty(nanoer::kDataStart, 20, options, io, 0, 0, 1);
    for (std::uint64_t i = 0; i < 16; ++i) m.segments[i] = {i + 1, 0};
    nanoer::Decoder ops(input);
    while (!ops.done()) m.apply(nanoer::decode_op(ops));
    // Whatever was accepted reads back, and survives a checkpoint.
    m.flush();
    nanoer::Metadata back =
        nanoer::Metadata::decode_checkpoint(m.encode_checkpoint(), nanoer::kDataStart, 20, options, io);
    if (!back.same_as(m)) __builtin_trap();
    (void)back.to_legacy();
  } catch (const nanoer::Error& e) {
    if (e.code() != nanoer::Errc::corrupt) __builtin_trap();
  }
  return 0;
}
