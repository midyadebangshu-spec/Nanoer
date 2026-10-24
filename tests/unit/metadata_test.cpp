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
