#include "nanoer/journal.h"

#include <memory>
#include <random>
#include <set>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "nanoer/error.h"
#include "nanoer/io.h"
#include "nanoer/mkfs.h"

using namespace nanoer;

namespace {

constexpr unsigned kShift = 20;  // 1 MiB segments, so tests cross many of them

// Mounts the log the way the filesystem does: replay, then resume.
struct Session {
  Store& store;
  Superblock sb;
  Metadata meta{0, kShift};
  std::unique_ptr<Journal> journal;
  ReplayResult replayed{Metadata(0, kShift), {}, {}, 0, 0, 0, 0};

  explicit Session(Store& s) : store(s), sb(load_superblock(s).sb) {
    replayed = replay(store, sb);
    meta = replayed.meta;
    journal = std::make_unique<Journal>(store, sb, meta,
                                        std::max(sb.next_segment_seq, replayed.max_seq + 1));
    journal->resume(replayed.end);
  }

  void commit(std::vector<Op> ops) {
    for (const Op& op : ops) meta.apply(op);
    std::uint64_t txid = meta.last_txid + 1;
    journal->append_transaction(txid, ops);
    meta.last_txid = txid;
  }

  // (Re)writes file `ino` as `chunks` chunks of random-ish data in one transaction.
  void write_file(InodeId ino, int chunks, std::uint64_t seed, std::size_t chunk_bytes = 100000) {
    std::vector<Op> ops;
    InodeAttrs a;
    a.id = ino;
    a.mode = S_IFREG | 0644;
    a.nlink = 1;
    a.size = chunk_bytes * static_cast<std::uint64_t>(chunks);
    ops.push_back(op::PutInode{a});
    ops.push_back(op::SetEntry{kRootInode, "f" + std::to_string(ino), ino});
    // Chunks must be appended before the transaction, and their SetChunk ops
    // can only be applied once the inode exists, so apply the first two now.
    for (const Op& op : ops) meta.apply(op);
    std::mt19937_64 rng(seed);
    std::vector<Op> chunk_ops;
    for (int c = 0; c < chunks; ++c) {
      Bytes data(chunk_bytes);
      for (auto& b : data) b = static_cast<std::uint8_t>(rng() % 64);  // compresses somewhat
      EncodedChunk enc = encode_chunk(data, CodecId::zstd, 1);
      ChunkRef ref = journal->append_chunk(enc, ino, static_cast<std::uint64_t>(c) * chunk_bytes);
      chunk_ops.push_back(op::SetChunk{ino, static_cast<std::uint64_t>(c), ref});
    }
    for (const Op& op : chunk_ops) meta.apply(op);
    ops.insert(ops.end(), chunk_ops.begin(), chunk_ops.end());
    std::uint64_t txid = meta.last_txid + 1;
    journal->append_transaction(txid, ops);
    meta.last_txid = txid;
  }

  void checkpoint() {
    LogPosition at = journal->append_checkpoint(meta);
    journal->flush();
    sb.checkpoint_offset = at.offset;
    sb.checkpoint_seq = at.seq;
    sb.next_segment_seq = journal->next_seq();
    write_superblock(store, sb);
    journal->checkpoint_done(at);
  }
};

std::unique_ptr<FaultStore> fresh_store(std::uint64_t size = 64 << 20) {
  auto store = std::make_unique<FaultStore>(size);
  make_filesystem(*store, {.segment_shift = kShift});
  store->flush();
  return store;
}

Metadata replayed_meta(Store& store) { return replay(store, load_superblock(store).sb).meta; }

}  // namespace

TEST(Replay, FreshFilesystemIsAnEmptyRoot) {
  auto store = fresh_store();
  ReplayResult r = replay(*store, load_superblock(*store).sb);
  ASSERT_EQ(r.meta.inodes.size(), 1u);
  EXPECT_TRUE(r.meta.find(kRootInode)->is_dir());
  EXPECT_EQ(r.transactions, 0u);
  EXPECT_EQ(r.end.index, 0u);
  EXPECT_EQ(r.meta.segments.size(), 1u);
}

TEST(Replay, TransactionsAcrossManySegments) {
  auto store = fresh_store();
  Metadata expected{0, kShift};
  {
    Session s(*store);
    for (InodeId ino = 2; ino < 32; ++ino) s.write_file(ino, 3, ino);
    s.journal->flush();
    expected = s.meta;
    EXPECT_GT(s.meta.segments.size(), 5u) << "test should cross segment boundaries";
  }
  EXPECT_EQ(replayed_meta(*store), expected);
}

TEST(Replay, RepeatedRemountsKeepEverything) {
  auto store = fresh_store();
  Metadata expected{0, kShift};
  for (int round = 0; round < 5; ++round) {
    Session s(*store);
    for (int i = 0; i < 4; ++i) s.write_file(static_cast<InodeId>(2 + round * 4 + i), 2, round * 10 + i);
    s.journal->flush();
    expected = s.meta;
  }
  Metadata back = replayed_meta(*store);
  EXPECT_EQ(back, expected);
  EXPECT_EQ(back.inodes.size(), 21u);
}

TEST(Replay, LargeCheckpointSpansSegments) {
  auto store = fresh_store();
  Metadata expected{0, kShift};
  {
    Session s(*store);
    std::vector<Op> ops;
    for (InodeId ino = 2; ino < 40000; ++ino) {
      InodeAttrs a;
      a.id = ino;
      a.mode = S_IFREG | 0600;
      a.nlink = 1;
      ops.push_back(op::PutInode{a});
      ops.push_back(op::SetEntry{kRootInode, "file-with-a-longish-name-" + std::to_string(ino), ino});
      if (ops.size() > 4000) {
        s.commit(std::move(ops));
        ops.clear();
      }
    }
    s.commit(std::move(ops));
    s.checkpoint();
    expected = s.meta;
  }
  ReplayResult r = replay(*store, load_superblock(*store).sb);
  EXPECT_GT(r.checkpoint_bytes, 1u << 20) << "checkpoint should be bigger than a segment";
  EXPECT_EQ(r.transactions, 0u);
  EXPECT_EQ(r.meta.inodes, expected.inodes);
  EXPECT_EQ(r.meta.last_txid, expected.last_txid);
  // The snapshot is taken before the segments it supersedes are released, so
  // after a remount those count as in use until the next checkpoint frees
  // them: replay knows at least every segment the live filesystem had.
  for (const auto& [index, usage] : expected.segments) {
    ASSERT_TRUE(r.meta.segments.contains(index));
    EXPECT_EQ(r.meta.segments.at(index), usage);
  }
}

TEST(Replay, DeadSegmentsAreReclaimedAndReused) {
  auto store = fresh_store();
  Metadata expected{0, kShift};
  {
    Session s(*store);
    for (InodeId ino = 2; ino < 12; ++ino) s.write_file(ino, 4, ino);
    s.checkpoint();
    std::set<std::uint64_t> first_round;
    for (const auto& [index, usage] : s.meta.segments) first_round.insert(index);

    for (InodeId ino = 2; ino < 12; ++ino) s.write_file(ino, 4, ino + 100);  // all old data dead
    s.checkpoint();                                // releases the dead segments...
    EXPECT_GT(s.journal->pending_free_segments(), 0u);
    s.checkpoint();                                // ...which become free only now
    EXPECT_EQ(s.journal->pending_free_segments(), 0u);

    for (InodeId ino = 12; ino < 30; ++ino) s.write_file(ino, 4, ino);
    s.journal->flush();
    bool reused = false;
    for (const auto& [index, usage] : s.meta.segments) {
      reused |= first_round.contains(index) && usage.seq > s.sb.checkpoint_seq;
    }
    EXPECT_TRUE(reused) << "freed segments should be written again";
    expected = s.meta;
  }
  EXPECT_EQ(replayed_meta(*store), expected);
}

TEST(Replay, RunsOutOfSpaceCleanly) {
  auto store = fresh_store(min_store_size(kShift) + (4 << 20));  // 8 segments
  Session s(*store);
  s.journal->set_reserve(2);
  try {
    for (InodeId ino = 2;; ++ino) s.write_file(ino, 2, ino, 200000);
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::no_space);
  }
  // The reserve is still there for a checkpoint.
  EXPECT_NO_THROW(s.journal->append_checkpoint(s.meta));
}

TEST(Replay, PowerCutAlwaysRecoversAStateThatExisted) {
  for (std::uint64_t seed = 0; seed < 12; ++seed) {
    auto store = fresh_store();
    std::vector<Metadata> states;  // every state from the last durable one on
    {
      Session s(*store);
      for (InodeId ino = 2; ino < 8; ++ino) s.write_file(ino, 2, ino);
      s.journal->flush();
      states.push_back(s.meta);
      // More transactions that reach the store but are never flushed.
      for (InodeId ino = 8; ino < 20; ++ino) {
        s.write_file(ino, 2, ino + seed);
        s.journal->write_buffered();
        states.push_back(s.meta);
      }
    }
    MemStore after(store->crash_image({.keep_probability = 0.6, .torn_writes = true, .seed = seed}));
    Metadata recovered = replayed_meta(after);
    bool matches = false;
    for (const Metadata& m : states) matches |= (recovered.inodes == m.inodes);
    EXPECT_TRUE(matches) << "seed " << seed << ": recovered " << recovered.inodes.size()
                         << " inodes, matching no real state";

    // And the recovered filesystem keeps working.
    Metadata expected{0, kShift};
    {
      Session s(after);
      s.write_file(100, 3, 7);
      s.journal->flush();
      expected = s.meta;
    }
    EXPECT_EQ(replayed_meta(after), expected) << "seed " << seed;
  }
}

TEST(Replay, MissingCheckpointIsCorrupt) {
  auto store = fresh_store();
  Superblock sb = load_superblock(*store).sb;
  sb.checkpoint_offset += 8;  // points at nothing
  write_superblock(*store, sb);
  try {
    replay(*store, load_superblock(*store).sb);
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.code(), Errc::corrupt);
  }
}
