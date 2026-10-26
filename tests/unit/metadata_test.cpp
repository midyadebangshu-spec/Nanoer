#include "nanoer/metadata.h"

#include <limits>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/encoding.h"
#include "nanoer/error.h"

using namespace nanoer;

namespace {

constexpr std::uint64_t kDataStart = 1 << 20;
constexpr unsigned kShift = 20;

std::uint64_t at(std::uint64_t segment, std::uint64_t within) {
  return kDataStart + (segment << kShift) + within;
}

InodeAttrs file_attrs(InodeId id) {
  InodeAttrs a;
  a.id = id;
  a.mode = S_IFREG | 0644;
  a.nlink = 1;
  a.uid = 1000;
  a.gid = 100;
  a.size = 300000;
  a.mtime_ns = -5;  // before 1970: signed times must survive
  return a;
}

Metadata sample() {
  Metadata m = Metadata::empty(kDataStart, kShift, 0, 0, 123);
  m.segments[0] = {1, 0};
  m.segments[3] = {7, 0};
  m.apply(op::PutInode{file_attrs(2)});
  m.apply(op::SetEntry{kRootInode, "file.txt", 2});
  m.apply(op::SetChunk{2, 0, {at(0, 4096), 1000, 131072}});
  m.apply(op::SetChunk{2, 2, {at(3, 4096), 500, 37856}});
  m.apply(op::SetXattr{2, "user.tag", {1, 2, 3}});
  InodeAttrs dir;
  dir.id = 3;
  dir.mode = S_IFDIR | 0700;
  dir.nlink = 2;
  dir.parent = kRootInode;
  m.apply(op::PutInode{dir});
  m.apply(op::SetEntry{kRootInode, "dir", 3});
  InodeAttrs link;
  link.id = 4;
  link.mode = S_IFLNK | 0777;
  link.nlink = 1;
  link.symlink = "../file.txt";
  m.apply(op::PutInode{link});
  m.apply(op::SetEntry{3, "link", 4});
  m.last_txid = 42;
  return m;
}

Op round_trip(const Op& op) {
  Encoder out;
  encode_op(out, op);
  Bytes bytes = out.take();
  Decoder in(bytes);
  Op back = decode_op(in);
  EXPECT_TRUE(in.done());
  return back;
}

}  // namespace

TEST(Encoding, VarintsRoundTrip) {
  Encoder out;
  const std::uint64_t values[] = {0, 1, 127, 128, 300, 1ull << 35, std::numeric_limits<std::uint64_t>::max()};
  const std::int64_t signed_values[] = {0, -1, 1, -64, 64, std::numeric_limits<std::int64_t>::min(),
                                        std::numeric_limits<std::int64_t>::max()};
  for (auto v : values) out.varint(v);
  for (auto v : signed_values) out.svarint(v);
  out.str("hello");
  Bytes b = out.take();
  Decoder in(b);
  for (auto v : values) EXPECT_EQ(in.varint(), v);
  for (auto v : signed_values) EXPECT_EQ(in.svarint(), v);
  EXPECT_EQ(in.str(10), "hello");
  EXPECT_TRUE(in.done());
}

TEST(Encoding, MalformedInputIsCorrupt) {
  Bytes too_long(11, 0x80);
  EXPECT_THROW(Decoder(too_long).varint(), Error);
  Bytes overflow = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02};
  EXPECT_THROW(Decoder(overflow).varint(), Error);
  Bytes truncated = {0x05, 'a', 'b'};
  EXPECT_THROW(Decoder(truncated).str(100), Error);
  Bytes oversized = {0x05, 'a', 'b', 'c', 'd', 'e'};
  EXPECT_THROW(Decoder(oversized).str(4), Error);
}

TEST(Ops, EveryOperationRoundTrips) {
  InodeAttrs a = file_attrs(9);
  a.symlink = "target";
  const Op ops[] = {
      op::PutInode{a},
      op::DeleteInode{9},
      op::SetEntry{1, "name", 9},
      op::RemoveEntry{1, "name"},
      op::SetChunk{9, 77, {at(2, 8), 4000, 131072}},
      op::RemoveChunk{9, 77},
      op::TruncateChunks{9, 3},
      op::SetXattr{9, "user.x", Bytes(1000, 7)},
      op::RemoveXattr{9, "user.x"},
  };
  for (const Op& op : ops) {
    Op back = round_trip(op);
    ASSERT_EQ(back.index(), op.index());
    Encoder a1, a2;
    encode_op(a1, op);
    encode_op(a2, back);
    EXPECT_EQ(a1.buffer(), a2.buffer());
  }
  Bytes unknown = {200};
  Decoder in(unknown);
  EXPECT_THROW(decode_op(in), Error);
}

TEST(Metadata, ApplyMaintainsSpaceAccounting) {
  Metadata m = sample();
  EXPECT_EQ(m.segments[0].live_bytes, 1000u);
  EXPECT_EQ(m.segments[3].live_bytes, 500u);

  m.apply(op::SetChunk{2, 0, {at(3, 9000), 700, 131072}});  // rewrite moves chunk 0
  EXPECT_EQ(m.segments[0].live_bytes, 0u);
  EXPECT_EQ(m.segments[3].live_bytes, 1200u);

  m.apply(op::TruncateChunks{2, 1});
  EXPECT_EQ(m.segments[3].live_bytes, 700u);
  EXPECT_EQ(m.find(2)->chunks.size(), 1u);

  m.apply(op::RemoveEntry{kRootInode, "file.txt"});
  m.apply(op::DeleteInode{2});
  EXPECT_EQ(m.segments[3].live_bytes, 0u);
  EXPECT_EQ(m.find(2), nullptr);
}

TEST(Metadata, InconsistentOperationsAreCorrupt) {
  Metadata m = sample();
  EXPECT_THROW(m.apply(op::SetEntry{kRootInode, "x", 99}), Error);         // missing inode
  EXPECT_THROW(m.apply(op::SetEntry{2, "x", 3}), Error);                   // not a directory
  EXPECT_THROW(m.apply(op::SetEntry{kRootInode, "a/b", 3}), Error);        // invalid name
  EXPECT_THROW(m.apply(op::SetEntry{kRootInode, "..", 3}), Error);
  EXPECT_THROW(m.apply(op::RemoveEntry{kRootInode, "nope"}), Error);
  EXPECT_THROW(m.apply(op::SetChunk{3, 0, {at(0, 0), 1, 1}}), Error);      // directory
  EXPECT_THROW(m.apply(op::SetChunk{2, 9, {at(5, 0), 1, 1}}), Error);      // segment not in use
  EXPECT_THROW(m.apply(op::DeleteInode{kRootInode}), Error);
  EXPECT_THROW(m.apply(op::RemoveXattr{2, "user.none"}), Error);
}

TEST(Metadata, PutInodeKeepsContentsAndAdvancesCounter) {
  Metadata m = sample();
  InodeAttrs a = file_attrs(2);
  a.size = 5;
  m.apply(op::PutInode{a});
  EXPECT_EQ(m.find(2)->size, 5u);
  EXPECT_EQ(m.find(2)->chunks.size(), 2u);  // attributes only; chunks untouched
  m.apply(op::PutInode{file_attrs(100)});
  EXPECT_EQ(m.next_inode, 101u);
}

TEST(Checkpoint, RoundTripIsExact) {
  Metadata m = sample();
  Bytes snapshot = m.encode_checkpoint();
  Metadata back = Metadata::decode_checkpoint(snapshot, kDataStart, kShift);
  EXPECT_EQ(back, m);
  EXPECT_EQ(back.encode_checkpoint(), snapshot);  // deterministic
}

TEST(Checkpoint, DamageIsDetected) {
  Bytes snapshot = sample().encode_checkpoint();
  int rejected = 0;
  for (std::size_t i = 0; i < snapshot.size(); ++i) {
    Bytes damaged = snapshot;
    damaged[i] ^= 0x41;
    try {
      Metadata::decode_checkpoint(damaged, kDataStart, kShift);
    } catch (const Error& e) {
      EXPECT_EQ(e.code(), Errc::corrupt);
      ++rejected;
    }
  }
  // Not every byte flip is detectable from structure alone (a changed
  // timestamp is still a valid timestamp); the checkpoint record's
  // checksums catch those. Most must be caught here as well.
  EXPECT_GT(rejected, static_cast<int>(snapshot.size() / 3));
  EXPECT_THROW(Metadata::decode_checkpoint(ByteView(snapshot).first(snapshot.size() - 1), kDataStart,
                                           kShift),
               Error);
}

// ---- shared records (M4) ---------------------------------------------------

namespace {

Metadata shared_sample() {
  Metadata m = Metadata::empty(kDataStart, kShift, 0, 0, 123, true);
  m.segments[0] = {1, 0};
  m.segments[1] = {2, 0};
  for (InodeId id = 2; id <= 4; ++id) {
    m.apply(op::PutInode{file_attrs(id)});
    m.apply(op::SetEntry{kRootInode, "f" + std::to_string(id), id});
  }
  return m;
}

ChunkRef hashed(std::uint64_t offset, std::uint32_t stored, std::uint32_t size, std::uint64_t key) {
  ChunkRef r{offset, stored, size};
  r.hash = key;
  return r;
}

ChunkRef piece(std::uint64_t pack, std::uint32_t stored, std::uint32_t where, std::uint32_t size) {
  ChunkRef r{pack, stored, size};
  r.packed = true;
  r.piece = where;
  return r;
}

}  // namespace

TEST(Ops, SetChunkRefRoundTrips) {
  ChunkRef p = piece(at(0, 64), 2000, 4096, 700);
  p.hash = 0x0123456789abcdefull;
  for (const Op& op : {Op{op::SetChunkRef{2, 0, p, false}},
                       Op{op::SetChunkRef{3, 5, hashed(at(1, 8), 900, 131072, 7), true}},
                       Op{op::SetChunkRef{4, 1, ChunkRef{at(1, 16), 100, 100}, false}}}) {
    Op back = round_trip(op);
    const auto& a = std::get<op::SetChunkRef>(op);
    const auto& b = std::get<op::SetChunkRef>(back);
    EXPECT_EQ(a.ino, b.ino);
    EXPECT_EQ(a.index, b.index);
    EXPECT_EQ(a.ref, b.ref);
    EXPECT_EQ(a.share, b.share);
  }
}

TEST(SharedRecords, SpaceIsCountedOncePerRecord) {
  Metadata m = shared_sample();
  ChunkRef r = hashed(at(0, 64), 1000, 131072, 99);
  m.apply(op::SetChunkRef{2, 0, r, false});
  m.apply(op::SetChunkRef{3, 0, r, true});
  m.apply(op::SetChunkRef{4, 7, r, true});
  EXPECT_EQ(m.segments[0].live_bytes, 1000u);
  EXPECT_EQ(m.references(r), 3u);

  // The last reference releases the space, whichever file goes first.
  m.apply(op::DeleteInode{2});
  EXPECT_EQ(m.references(r), 2u);
  m.apply(op::RemoveChunk{3, 0});
  EXPECT_EQ(m.references(r), 1u);
  EXPECT_FALSE(m.shared.contains(r.offset));  // one reference left: not counted
  EXPECT_EQ(m.segments[0].live_bytes, 1000u);
  m.apply(op::TruncateChunks{4, 0});
  EXPECT_EQ(m.segments[0].live_bytes, 0u);
}

TEST(SharedRecords, PacksAreReleasedByTheirLastPiece) {
  Metadata m = shared_sample();
  m.apply(op::SetChunkRef{2, 0, piece(at(1, 64), 3000, 0, 500), false});
  m.apply(op::SetChunkRef{3, 0, piece(at(1, 64), 3000, 500, 9000), false});
  m.apply(op::SetChunkRef{4, 0, piece(at(1, 64), 3000, 500, 9000), false});  // a duplicate piece
  EXPECT_EQ(m.segments[1].live_bytes, 3000u);
  EXPECT_EQ(m.shared.at(at(1, 64)), 3u);
  m.apply(op::DeleteInode{3});
  m.apply(op::DeleteInode{4});
  EXPECT_EQ(m.shared.at(at(1, 64)), 1u);  // a pack stays counted down to one
  // Rewriting the last piece's file as an ordinary chunk frees the pack.
  m.apply(op::SetChunk{2, 0, {at(0, 64), 400, 500}});
  EXPECT_FALSE(m.shared.contains(at(1, 64)));
  EXPECT_EQ(m.segments[1].live_bytes, 0u);
  EXPECT_EQ(m.segments[0].live_bytes, 400u);
}

TEST(SharedRecords, MovingASharedRecordReleasesTheOldOnce) {
  // What GC does: the first reference moves to a new record, the others
  // share it.
  Metadata m = shared_sample();
  ChunkRef old_ref = hashed(at(0, 64), 1000, 131072, 99);
  for (InodeId id = 2; id <= 4; ++id) m.apply(op::SetChunkRef{id, 0, old_ref, id != 2});
  ChunkRef new_ref = hashed(at(1, 128), 1000, 131072, 99);
  for (InodeId id = 2; id <= 4; ++id) m.apply(op::SetChunkRef{id, 0, new_ref, id != 2});
  EXPECT_EQ(m.segments[0].live_bytes, 0u);
  EXPECT_EQ(m.segments[1].live_bytes, 1000u);
  EXPECT_FALSE(m.shared.contains(old_ref.offset));
  EXPECT_EQ(m.shared.at(new_ref.offset), 3u);
}

TEST(SharedRecords, CheckpointRoundTripKeepsCountsAndFields) {
  Metadata m = shared_sample();
  ChunkRef r = hashed(at(0, 64), 1000, 131072, 99);
  m.apply(op::SetChunkRef{2, 0, r, false});
  m.apply(op::SetChunkRef{3, 0, r, true});
  ChunkRef p = piece(at(1, 64), 3000, 512, 77);
  p.hash = 1234;
  m.apply(op::SetChunkRef{4, 0, p, false});
  m.apply(op::SetChunk{4, 1, {at(1, 4096), 200, 131072}});
  Bytes snapshot = m.encode_checkpoint();
  EXPECT_TRUE(Metadata::is_v2_checkpoint(snapshot));
  Metadata back = Metadata::decode_checkpoint(snapshot, kDataStart, kShift, true);
  EXPECT_EQ(back, m);
  EXPECT_EQ(back.find(4)->chunks.at(0), p);
  // A filesystem without the feature refuses such a checkpoint.
  EXPECT_THROW(Metadata::decode_checkpoint(snapshot, kDataStart, kShift, false), Error);
}

TEST(SharedRecords, OldFilesystemsStayAsTheyWere) {
  // Without the feature: version 1 checkpoints, and no shared references.
  Metadata m = sample();
  EXPECT_FALSE(Metadata::is_v2_checkpoint(m.encode_checkpoint()));
  ChunkRef r = m.find(2)->chunks.at(0);
  EXPECT_THROW(m.apply(op::SetChunkRef{2, 5, r, true}), Error);
  ChunkRef with_hash = r;
  with_hash.hash = 5;
  EXPECT_THROW(m.apply(op::SetChunkRef{2, 5, with_hash, false}), Error);
  // A filesystem with the feature still reads a version 1 checkpoint.
  Metadata back = Metadata::decode_checkpoint(sample().encode_checkpoint(), kDataStart, kShift, true);
  EXPECT_TRUE(back.shared_records());
  EXPECT_EQ(back.inodes, sample().inodes);
}

TEST(SharedRecords, InconsistentReferencesAreCorrupt) {
  Metadata m = shared_sample();
  // A new record where a shared one already is.
  ChunkRef r = hashed(at(0, 64), 1000, 131072, 99);
  m.apply(op::SetChunkRef{2, 0, r, false});
  m.apply(op::SetChunkRef{3, 0, r, true});
  EXPECT_THROW(m.apply(op::SetChunkRef{4, 0, r, false}), Error);
  // SetChunk cannot carry the extended fields, and only packs have pieces.
  EXPECT_THROW(m.apply(op::SetChunk{2, 3, piece(at(1, 0), 10, 0, 5)}), Error);
  ChunkRef stray{at(1, 8), 10, 5};
  stray.piece = 3;
  EXPECT_THROW(m.apply(op::SetChunkRef{2, 4, stray, false}), Error);

  // A checkpoint whose references to one record disagree.
  Metadata n = shared_sample();
  n.apply(op::SetChunkRef{2, 0, r, false});
  n.apply(op::SetChunkRef{3, 0, r, true});
  n.find(3)->chunks.at(0).stored_size = 999;
  EXPECT_THROW(Metadata::decode_checkpoint(n.encode_checkpoint(), kDataStart, kShift, true), Error);
}
