#include "check_tree.h"

#include <algorithm>
#include <list>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/stat.h>

#include "meta_encoding.h"
#include "nanoer/checksum.h"
#include "nanoer/error.h"
#include "nanoer/record.h"

namespace nanoer {

namespace {

using meta::Refs;

std::string range(std::uint64_t first, std::uint64_t length) {
  return "bytes " + std::to_string(first) + "-" + std::to_string(first + length - 1);
}

enum Type : std::uint8_t { kNone = 0, kDir, kFile, kLink, kOther };

Type type_of(std::uint32_t mode) {
  if (S_ISDIR(mode)) return kDir;
  if (S_ISREG(mode)) return kFile;
  if (S_ISLNK(mode)) return kLink;
  return kOther;
}

const char* kind_name(Type t) {
  switch (t) {
    case kDir: return "directory";
    case kFile: return "file";
    case kLink: return "symlink";
    default: return "special file";
  }
}

TreeKey key(InodeId object, std::uint8_t kind, std::uint64_t offset = 0) { return {object, kind, offset}; }

bool by_user(const ChunkUser& a, const ChunkUser& b) { return std::pair(a.ino, a.index) < std::pair(b.ino, b.index); }

class TreeCheck {
 public:
  explicit TreeCheck(const TreeCheckInput& in)
      : store_(in.store),
        sb_(in.sb),
        options_(in.options),
        report_(in.report),
        meta_(in.meta),
        node_bytes_(in.node_bytes),
        repair_(in.options.repair) {}

  bool run() {
    contents();
    structure();
    chunk_messages();
    if (repair_ && (changed_ || derived_wrong_)) rebuild_derived();
    return changed_;
  }

 private:
  // ---- what is kept per inode and per entry --------------------------------

  struct Ino {
    InodeId id = 0;
    InodeId parent = 0;         // as recorded (directories)
    InodeId named_by = 0;       // the directory whose entry keeps it (directories)
    std::uint64_t name_hash = 0;
    std::uint32_t nlink = 0;
    std::uint32_t names = 0;    // live entries naming it
    std::uint32_t subdirs = 0;  // live entries in it naming directories
    Type type = kNone;
    std::uint8_t reach = 0;  // 0 not known, 1 being found, 2 reachable from /, 3 not
    bool gone = false;       // deleted by a repair
  };
  struct Ent {
    InodeId dir;
    InodeId child;  // 0: removed by a repair
    std::uint64_t hash;
  };

  Ino* find(InodeId id) {
    auto it = std::lower_bound(inodes_.begin(), inodes_.end(), id, [](const Ino& i, InodeId v) { return i.id < v; });
    return it != inodes_.end() && it->id == id && !it->gone ? &*it : nullptr;
  }

  void error(const std::string& m) { report_.errors.push_back(m); }
  void note(const std::string& m) { report_.notes.push_back(m); }
  void derived(const std::string& m) {
    error(m);
    derived_wrong_ = true;
  }

  // ---- pass 1: every item, in key order -------------------------------------

  struct Current {
    bool valid = false;
    InodeAttrs attrs;
    InodeCounts seen;    // from its entries and chunks
    InodeCounts stored;  // its counts item
  };

  void contents() {
    // Exactly as many as the totals say (if they are sane), not twice that
    // by doubling.
    std::uint64_t inodes = meta_.counters.files + meta_.counters.directories + meta_.counters.other_inodes;
    if (inodes < (std::uint64_t{1} << 32)) {
      inodes_.reserve(inodes + 16);
      entries_.reserve(inodes + 16);
    }
    meta_.scan({}, [&](const TreeKey& k, ByteView v) {
      item(k, v);
      return true;
    });
    finish_inode();
    if (bad_items_) {
      error(std::to_string(bad_items_) + " metadata items are damaged or misplaced (" + first_bad_ + ")");
    }
    if (bad_counts_) derived(std::to_string(bad_counts_) + " inodes' entry or data counts are wrong");

    // Unlinked inodes: exactly those with no links left.
    std::vector<InodeId> want;
    for (const Ino& i : inodes_) {
      if (i.nlink == 0 && i.id != kRootInode) want.push_back(i.id);
    }
    std::sort(listed_orphans_.begin(), listed_orphans_.end());
    if (listed_orphans_ != want) derived("the list of unlinked inodes is wrong");

    // Back-references against the chunks that refer to each record.
    std::size_t bad_refs = flagged_.size();
    for (auto& [offset, r] : refs_) {
      std::sort(r.users.begin(), r.users.end(), by_user);
      auto it = shared_.find(offset);
      std::vector<ChunkUser> seen = it != shared_.end() ? it->second.users : std::vector<ChunkUser>{};
      std::sort(seen.begin(), seen.end(), by_user);
      if (seen != r.users) ++bad_refs;
    }
    for (const auto& [offset, s] : shared_) {
      if (!refs_.contains(offset)) ++bad_refs;  // a pack without its list
    }
    if (bad_refs) derived(std::to_string(bad_refs) + " records' lists of users are wrong");

    // Totals, and the space of every segment: each record once, and nodes.
    for (const auto& [index, bytes] : node_bytes_) want_.node_bytes += bytes;
    if (!(meta_.counters == want_)) derived("the filesystem's totals (files, directories, sizes, tree size) are wrong");
    std::map<std::uint64_t, std::uint64_t> live = live_;
    for (const auto& [offset, s] : shared_) live[meta_.segment_of(offset)] += s.first.stored_size;
    for (const auto& [index, bytes] : node_bytes_) live[index] += bytes;
    bool space_wrong = false;
    for (const auto& [index, usage] : meta_.segments) {
      if (usage.live_bytes != (live.contains(index) ? live[index] : 0)) space_wrong = true;
    }
    if (space_wrong) derived("the space accounting of segments is wrong");
  }

  void bad_item(const TreeKey& k, const std::string& why) {
    if (bad_items_++ == 0) first_bad_ = why;
    if (repair_) bad_keys_.push_back(k);
  }

  void item(const TreeKey& k, ByteView v) {
    try {
      if (k.kind == item_kind::refs) {
        if (!(k.object & kRefsObject) || k.offset != 0) return bad_item(k, "a misplaced refs item");
        Refs r = meta::decode_refs(v);
        std::set<std::pair<InodeId, std::uint64_t>> once;
        for (const ChunkUser& u : r.users) {
          if (!once.insert({u.ino, u.index}).second) return bad_item(k, "a refs item naming a user twice");
        }
        refs_.emplace(k.object & ~kRefsObject, std::move(r));
        return;
      }
      if (k.object & kRefsObject) return bad_item(k, "an item past the inodes");
      if (k.kind == item_kind::orphan) {
        if (k.object != 0 || !v.empty()) return bad_item(k, "a misplaced orphan item");
        listed_orphans_.push_back(k.offset);
        return;
      }
      if (k.object == 0) return bad_item(k, "an item of inode 0");
      if (k.kind == item_kind::inode) {
        finish_inode();
        if (k.offset != 0) return bad_item(k, "a misplaced inode item");
        InodeAttrs a = meta::decode_inode(v);
        if (a.id != k.object) return bad_item(k, "an inode item under another number");
        if (!inodes_.empty() && inodes_.back().id >= a.id) return bad_item(k, "inode items out of order");
        cur_ = Current{};
        cur_.valid = true;
        cur_.attrs = a;
        Ino i;
        i.id = a.id;
        i.parent = a.parent;
        i.nlink = a.nlink;
        i.type = type_of(a.mode);
        inodes_.push_back(i);
        return;
      }
      if (!cur_.valid || cur_.attrs.id != k.object) return bad_item(k, "an item of an inode that does not exist");
      switch (k.kind) {
        case item_kind::counts:
          if (k.offset != 0) return bad_item(k, "a misplaced counts item");
          cur_.stored = meta::decode_counts(v);
          return;
        case item_kind::xattr: {
          std::set<std::string> names;
          for (const auto& [name, value] : meta::decode_xattrs(v)) {
            if (name_hash(name) != k.offset || !names.insert(name).second) {
              return bad_item(k, "an xattr item filed under the wrong name, or naming one twice");
            }
          }
          return;
        }
        case item_kind::entry: {
          if (type_of(cur_.attrs.mode) != kDir) return bad_item(k, "directory entries in something not a directory");
          auto list = meta::decode_entries(v);
          std::set<std::string> names;
          for (const auto& [name, child] : list) {
            if (name_hash(name) != k.offset || !names.insert(name).second || !meta::valid_name(name)) {
              return bad_item(k, "an entry item filed under the wrong name, or naming one twice");
            }
          }
          for (const auto& [name, child] : list) {
            entries_.push_back({k.object, child, k.offset});
            ++cur_.seen.entries;
          }
          return;
        }
        case item_kind::chunk:
          if (type_of(cur_.attrs.mode) != kFile) return bad_item(k, "file data in something not a file");
          chunk(k.offset, meta::decode_chunk_item(v));
          return;
        default:
          return bad_item(k, "an item of unknown kind");
      }
    } catch (const Error& e) {
      if (e.code() != Errc::corrupt) throw;
      bad_item(k, std::string("an item that cannot be decoded: ") + e.what());
    }
  }

  void finish_inode() {
    if (!cur_.valid) return;
    const InodeAttrs& a = cur_.attrs;
    if (!(cur_.seen == cur_.stored)) ++bad_counts_;
    switch (type_of(a.mode)) {
      case kFile:
        ++want_.files;
        want_.logical_bytes += a.size;
        break;
      case kDir:
        ++want_.directories;
        break;
      default:
        ++want_.other_inodes;
    }
    if (type_of(a.mode) == kLink && a.size != a.symlink.size()) {
      error("inode " + std::to_string(a.id) + ": symlink size does not match its target");
      if (repair_) {
        InodeAttrs fixed = a;
        fixed.size = a.symlink.size();
        put_attrs(fixed);
      }
    }
    cur_.valid = false;
  }

  // ---- chunks ----------------------------------------------------------------

  struct Shared {
    ChunkRef first;  // the first reference seen
    std::vector<ChunkUser> users;
  };

  struct RecordCheck {
    std::string problem;
    DataChunkInfo info;
    std::uint64_t stored_size = 0;
    std::optional<Bytes> data;  // with --deep, for packs (pieces are hashed)
  };

  void chunk(std::uint64_t index, const ChunkRef& ref) {
    const InodeId id = cur_.attrs.id;
    const std::uint64_t cs = sb_.chunk_size();
    ++report_.chunks;
    cur_.seen.data_bytes += ref.size;
    // Data past the end cannot be read (nothing is lost by dropping it),
    // but until then it holds its space.
    const bool past_end = index * cs >= cur_.attrs.size;
    if (past_end) problems_.push_back({id, index, ref.size, "a chunk lies past the end of the file", false});
    // Packs and records shared by several files list their users (refs
    // items); any other record names its one user in its header.
    if (ref.offset != users_offset_) {
      users_offset_ = ref.offset;
      users_ = meta_.users(ref.offset);
    }
    const bool listed = users_.has_value();
    const bool shared = ref.packed || listed;
    std::string problem;
    if (shared) {
      Shared& s = shared_[ref.offset];
      if (s.users.empty()) {
        s.first = ref;
      } else if (s.first.stored_size != ref.stored_size || s.first.packed != ref.packed) {
        problem = "disagrees with another file about its data";
      }
      s.users.push_back({id, index});
    }
    if (past_end) {
      if (!shared) live_[meta_.segment_of(ref.offset)] += ref.stored_size;
      return;
    }
    const RecordCheck& c = check_record(ref, shared);
    if (problem.empty()) problem = c.problem;
    if (problem.empty()) problem = check_fields(c, ref);
    if (problem.empty() && !ref.packed) {
      const bool owner = !c.info.pack && c.info.inode == id && c.info.file_offset == index * cs;
      const bool identity = ref.hash && c.info.blake3 && hash_key(c.info.blake3->data()) == ref.hash;
      if (!owner && !identity) {
        // Data without an identity belongs to the one chunk its header
        // names; only data with one is shared (dedup).
        problem = "its data belongs to another file";
      } else if (!owner && !shared) {
        // Shared by dedup, but the record has lost its list of users.
        flagged_.insert(ref.offset);
        return;
      }
    }
    if (!problem.empty()) {
      problems_.push_back({id, index, ref.size, problem, true});
      if (shared) {
        // Not one of its users after all.
        std::vector<ChunkUser>& u = shared_[ref.offset].users;
        u.pop_back();
        if (u.empty()) shared_.erase(ref.offset);
      }
      return;
    }
    if (!shared) live_[meta_.segment_of(ref.offset)] += ref.stored_size;
  }

  // What the record a reference points to says. Records with several
  // users are kept (a few thousand at most); others are read once.
  const RecordCheck& check_record(const ChunkRef& ref, bool keep) {
    if (auto it = records_.find(ref.offset); it != records_.end()) return it->second;
    RecordCheck c;
    std::uint64_t seg = meta_.segment_of(ref.offset);
    auto usage = meta_.segments.find(seg);
    std::optional<Record> r;
    if (ref.offset < sb_.data_start || seg >= sb_.segment_count || usage == meta_.segments.end()) {
      c.problem = "its data is in a free segment";
    } else {
      std::uint64_t limit = sb_.segment_offset(seg) + sb_.segment_size();
      r = options_.deep ? try_read_record(store_, ref.offset, limit, usage->second.seq)
                        : try_read_record_header(store_, ref.offset, limit, usage->second.seq);
      if (!r || r->header.type != RecordType::data_chunk) c.problem = "its data is missing or damaged";
    }
    if (c.problem.empty()) {
      try {
        c.info = parse_data_chunk(*r);
        c.stored_size = r->stored_size();
        if (options_.deep) {
          Bytes data = decode_data_chunk(*r);
          if (c.info.blake3 && blake3(data) != *c.info.blake3) throw Error(Errc::corrupt, "identity");
          if (c.info.pack) c.data = std::move(data);
          ++report_.chunks_verified;
        }
      } catch (const Error&) {
        c.problem = "its data is damaged";
      }
    }
    scratch_ = std::move(c);
    if (!keep) return scratch_;
    order_.push_back(ref.offset);
    if (order_.size() > kKeptRecords) {
      records_.erase(order_.front());
      order_.pop_front();
    }
    return records_.emplace(ref.offset, std::move(scratch_)).first->second;
  }

  std::string check_fields(const RecordCheck& c, const ChunkRef& ref) const {
    const DataChunkInfo& info = c.info;
    if (info.pack != ref.packed) return "its data is not what it was written as";
    if (c.stored_size != ref.stored_size) return "its size does not match its data";
    if (ref.packed) {
      if (std::uint64_t{ref.piece} + ref.size > info.uncompressed_size) return "it lies outside its pack";
    } else if (info.uncompressed_size != ref.size) {
      return "its size does not match its data";
    }
    if (ref.hash && !ref.packed && (!info.blake3 || hash_key(info.blake3->data()) != ref.hash)) {
      return "its identity does not match its data";
    }
    if (ref.hash && ref.packed && c.data) {
      Blake3Hash h = blake3(ByteView(*c.data).subspan(ref.piece, ref.size));
      if (hash_key(h.data()) != ref.hash) return "its identity does not match its data";
    }
    return {};
  }

  // ---- pass 2: directories, names, links ------------------------------------

  void structure() {
    Ino* root = find(kRootInode);
    if (!root || root->type != kDir) {
      error("the root directory is missing");
      if (!repair_) return;
      InodeAttrs a;
      a.id = kRootInode;
      a.mode = S_IFDIR | 0755;
      a.nlink = 2;
      a.parent = kRootInode;
      put_attrs(a);
      if (root) {
        root->type = kDir;
      } else {
        Ino i;
        i.id = kRootInode;
        i.parent = kRootInode;
        i.nlink = 2;
        i.type = kDir;
        inodes_.insert(inodes_.begin(), i);
      }
      root = find(kRootInode);
    }
    if (root->parent != kRootInode) {
      error("the root directory's parent is wrong");
      set_parent(*root, kRootInode);
    }

    // Entries to inodes that do not exist; each directory kept by one
    // entry (the one in the directory it records as its parent, if any).
    for (Ent& e : entries_) {
      Ino* c = find(e.child);
      if (!c) {
        error(in_dir(e) + " points at inode " + std::to_string(e.child) + ", which does not exist");
        remove_entry(e);
        continue;
      }
      if (c->type != kDir) continue;
      if (e.child == kRootInode) {
        error("directory / also appears as " + in_dir(e) + " (a directory may have only one name)");
        remove_entry(e);
        continue;
      }
      if (c->named_by == 0 || (c->named_by != c->parent && e.dir == c->parent)) {
        c->named_by = e.dir;
        c->name_hash = e.hash;
      }
    }
    for (Ent& e : entries_) {
      if (!e.child) continue;
      Ino* c = find(e.child);
      if (c->type == kDir && (c->named_by != e.dir || c->name_hash != e.hash)) {
        error("directory " + path_of(e.child) + " also appears as " + in_dir(e) + " (a directory may have only one name)");
        remove_entry(e);
      }
    }
    for (Ino& i : inodes_) {
      if (i.type == kDir && i.named_by && i.parent != i.named_by && !i.gone) {
        error("directory " + path_of(i.id) + " has the wrong parent");
        set_parent(i, i.named_by);
      }
    }

    // Orphans: deleted while still open when the system went down.
    find_reachable();
    std::uint64_t orphans = 0;
    for (Ino& i : inodes_) {
      if (!i.gone && i.reach != 2 && i.nlink == 0) {
        ++orphans;
        if (repair_) delete_inode(i);
      }
    }
    if (orphans) {
      note(std::to_string(orphans) + " files deleted while still open " +
           (repair_ ? "were freed" : "will be freed at the next mount"));
    }

    // Lost inodes: linked, but in no directory reachable from /. Only the
    // top of each lost subtree moves to lost+found; the rest comes along.
    std::unordered_set<InodeId> reported;
    for (int round = 0; round < 1000; ++round) {
      find_reachable();
      std::vector<InodeId> lost, tops;
      for (const Ino& i : inodes_) {
        if (!i.gone && i.reach != 2 && i.nlink != 0 && !reported.contains(i.id)) lost.push_back(i.id);
      }
      if (lost.empty()) break;
      count_names();
      for (InodeId id : lost) {
        const Ino& i = *find(id);
        if (i.type == kDir ? i.named_by == 0 : i.names == 0) tops.push_back(id);
      }
      if (tops.empty()) tops.push_back(lost.front());  // directories naming each other in a ring
      for (InodeId id : tops) {
        reported.insert(id);
        error(std::string(kind_name(find(id)->type)) + " " + std::to_string(id) + " is in no directory reachable from /");
        lost_and_found(id);
      }
      if (!repair_) break;  // nothing moves: the tops are reported once
    }

    // Link counts, now that the tree is settled.
    count_names();
    for (Ino& i : inodes_) {
      if (i.gone || i.reach != 2) continue;
      std::uint32_t expected = i.type == kDir ? 2 + i.subdirs : i.names;
      if (i.nlink != expected) {
        error(path_of(i.id) + ": link count is " + std::to_string(i.nlink) + ", should be " + std::to_string(expected));
        if (repair_) {
          std::optional<InodeAttrs> a = meta_.attrs(i.id);
          a->nlink = expected;
          put_attrs(*a);
          i.nlink = expected;
        }
      }
      if (i.type == kFile) ++report_.files;
      if (i.type == kDir) ++report_.directories;
    }
  }

  void count_names() {
    for (Ino& i : inodes_) i.names = i.subdirs = 0;
    for (const Ent& e : entries_) {
      if (!e.child) continue;
      Ino* c = find(e.child);
      Ino* d = find(e.dir);
      if (!c || !d) continue;
      ++c->names;
      if (c->type == kDir) ++d->subdirs;
    }
  }

  // Directories: reachable if the chain of directories keeping them leads
  // to /. Anything else: if a reachable directory names it.
  void find_reachable() {
    for (Ino& i : inodes_) i.reach = 0;
    Ino* root = find(kRootInode);
    if (root) root->reach = 2;
    std::vector<Ino*> chain;
    for (Ino& i : inodes_) {
      if (i.gone || i.type != kDir || i.reach) continue;
      chain.clear();
      Ino* at = &i;
      std::uint8_t result = 3;
      while (true) {
        if (at->reach == 2 || at->reach == 3) {
          result = at->reach;
          break;
        }
        if (at->reach == 1 || at->named_by == 0) break;  // a ring, or nothing keeps it
        at->reach = 1;
        chain.push_back(at);
        Ino* up = find(at->named_by);
        if (!up) break;
        at = up;
      }
      for (Ino* c : chain) c->reach = result;
    }
    for (const Ent& e : entries_) {
      if (!e.child) continue;
      Ino* c = find(e.child);
      Ino* d = find(e.dir);
      if (c && d && c->type != kDir && d->reach == 2) c->reach = 2;
    }
    for (Ino& i : inodes_) {
      if (!i.reach) i.reach = 3;
    }
  }

  void lost_and_found(InodeId id) {
    if (!repair_) return;
    if (!lost_found_) {
      std::optional<InodeId> existing = meta_.entry(kRootInode, "lost+found");
      if (existing && find(*existing) && find(*existing)->type == kDir) {
        lost_found_ = *existing;
      } else {
        InodeAttrs a;
        a.id = meta_.next_inode++;
        a.mode = S_IFDIR | 0700;
        a.nlink = 2;
        a.parent = kRootInode;
        a.atime_ns = a.mtime_ns = a.ctime_ns = a.btime_ns = static_cast<std::int64_t>(now_ns());
        put_attrs(a);
        Ino i;
        i.id = a.id;
        i.parent = kRootInode;
        i.nlink = 2;
        i.type = kDir;
        inodes_.push_back(i);  // the largest number: stays sorted
        add_entry(kRootInode, existing ? "lost+found.fsck" : "lost+found", a.id);
        lost_found_ = a.id;
      }
    }
    for (Ent& e : entries_) {
      if (e.child == id) remove_entry(e);
    }
    std::string name = "#" + std::to_string(id);
    add_entry(*lost_found_, name, id);
    Ino& i = *find(id);
    if (i.type == kDir) set_parent(i, *lost_found_);
    report_.fixed.push_back("reconnected inode " + std::to_string(id) + " as /lost+found/" + name);
  }

  // ---- names, for messages ----------------------------------------------------

  // "/name" of entry `e` in its directory.
  std::string entry_name(const Ent& e) {
    std::optional<Bytes> v = meta_.tree().get(key(e.dir, item_kind::entry, e.hash));
    if (v) {
      try {
        for (const auto& [name, child] : meta::decode_entries(*v)) {
          if (child == e.child) return "/" + name;
        }
      } catch (const Error&) {
      }
    }
    return "/?";
  }

  // The path of entry `e`.
  std::string in_dir(const Ent& e) {
    std::string up = path_of(e.dir);
    return (up == "/" ? "" : up) + entry_name(e);
  }

  std::string path_of(InodeId id) {
    if (id == kRootInode) return "/";
    if (auto it = paths_.find(id); it != paths_.end()) return it->second;
    std::string path = "inode " + std::to_string(id);
    Ino* i = find(id);
    if (i && i->type == kDir && i->named_by) {
      std::string up = path_of_dir(i->named_by, 0);
      if (!up.empty()) path = (up == "/" ? "" : up) + entry_name({i->named_by, id, i->name_hash});
    } else if (i) {
      for (const Ent& e : entries_) {
        if (e.child != id) continue;
        std::string up = path_of_dir(e.dir, 0);
        if (!up.empty()) {
          path = (up == "/" ? "" : up) + entry_name(e);
          break;
        }
      }
    }
    if (paths_.size() < 10000) paths_.emplace(id, path);
    return path;
  }
  // The path of a directory through the entries that keep it; empty if
  // that leads nowhere.
  std::string path_of_dir(InodeId id, int depth) {
    if (id == kRootInode) return "/";
    Ino* i = find(id);
    if (!i || !i->named_by || depth > 4096) return {};
    std::string up = path_of_dir(i->named_by, depth + 1);
    if (up.empty()) return {};
    return (up == "/" ? "" : up) + entry_name({i->named_by, id, i->name_hash});
  }

  void chunk_messages() {
    const std::uint64_t cs = sb_.chunk_size();
    for (const ChunkProblem& p : problems_) {
      std::string where = path_of(p.ino) + ", " + range(p.index * cs, p.size);
      error(p.lost ? where + ": " + p.problem : path_of(p.ino) + ": " + p.problem);
      if (p.lost) report_.lost.push_back(path_of(p.ino) + ": " + range(p.index * cs, p.size));
      if (repair_) erase(key(p.ino, item_kind::chunk, p.index));
    }
    for (const TreeKey& k : bad_keys_) erase(k);
  }

  // ---- repairs: items changed in the tree ------------------------------------

  void erase(const TreeKey& k) {
    if (meta_.tree().erase(k)) changed_ = true;
  }

  void put_attrs(const InodeAttrs& a) {
    meta_.tree().put(key(a.id, item_kind::inode), meta::encode_inode(a));
    changed_ = true;
  }

  void set_parent(Ino& i, InodeId parent) {
    i.parent = parent;
    if (!repair_) return;
    std::optional<InodeAttrs> a = meta_.attrs(i.id);
    if (!a) return;
    a->parent = parent;
    put_attrs(*a);
  }

  void remove_entry(Ent& e) {
    if (repair_) {
      TreeKey k = key(e.dir, item_kind::entry, e.hash);
      if (std::optional<Bytes> v = meta_.tree().get(k)) {
        auto list = meta::decode_entries(*v);
        std::erase_if(list, [&](const auto& n) { return n.second == e.child; });
        if (list.empty()) {
          meta_.tree().erase(k);
        } else {
          meta_.tree().put(k, meta::encode_entries(list));
        }
        changed_ = true;
      }
    }
    drop(e);
  }

  // An entry gone: a directory it kept is kept by nothing now.
  void drop(Ent& e) {
    if (Ino* c = find(e.child); c && c->type == kDir && c->named_by == e.dir && c->name_hash == e.hash) {
      c->named_by = 0;
    }
    e.child = 0;
  }

  void add_entry(InodeId dir, const std::string& name, InodeId child) {
    std::uint64_t h = name_hash(name);
    TreeKey k = key(dir, item_kind::entry, h);
    std::vector<std::pair<std::string, InodeId>> list;
    if (std::optional<Bytes> v = meta_.tree().get(k)) list = meta::decode_entries(*v);
    std::erase_if(list, [&](const auto& n) { return n.first == name; });
    list.emplace_back(name, child);
    meta_.tree().put(k, meta::encode_entries(list));
    entries_.push_back({dir, child, h});
    Ino* c = find(child);
    if (c && c->type == kDir) {
      c->named_by = dir;
      c->name_hash = h;
    }
    changed_ = true;
  }

  void delete_inode(Ino& i) {
    std::vector<TreeKey> keys;
    meta_.tree().scan(key(i.id, 0, 0), key(i.id, 0xff, UINT64_MAX), [&](const TreeKey& k, ByteView) {
      keys.push_back(k);
      return true;
    });
    for (const TreeKey& k : keys) meta_.tree().erase(k);
    for (Ent& e : entries_) {
      if (e.dir == i.id) drop(e);
    }
    for (Ent& e : entries_) {
      if (e.child == i.id) remove_entry(e);  // in an unreachable directory
    }
    i.gone = true;
    changed_ = true;
  }

  // ---- pass 3 (repairs): what is derived from the items, written afresh --------

  void rebuild_derived() {
    // Records with users to list: packs, records with a list already, and
    // records found shared without one.
    std::map<std::uint64_t, Shared> shared;
    std::map<std::uint64_t, std::uint64_t> live;
    std::set<std::uint64_t> had_refs;
    std::vector<InodeId> orphans;
    MetaCounters totals;
    struct Fix {
      InodeId ino;
      InodeCounts counts;
    };
    std::vector<Fix> counts;
    Current cur;
    auto finish = [&] {
      if (!cur.valid) return;
      if (!(cur.seen == cur.stored)) counts.push_back({cur.attrs.id, cur.seen});
      const InodeAttrs& a = cur.attrs;
      if (S_ISREG(a.mode)) {
        ++totals.files;
        totals.logical_bytes += a.size;
      } else if (S_ISDIR(a.mode)) {
        ++totals.directories;
      } else {
        ++totals.other_inodes;
      }
      if (a.nlink == 0 && a.id != kRootInode) orphans.push_back(a.id);
      cur.valid = false;
    };
    std::vector<InodeId> listed;
    meta_.scan({}, [&](const TreeKey& k, ByteView v) {
      if (k.kind == item_kind::refs) {
        had_refs.insert(k.object & ~kRefsObject);
      } else if (k.kind == item_kind::orphan) {
        listed.push_back(k.offset);
      } else if (k.kind == item_kind::inode) {
        finish();
        cur = Current{};
        cur.valid = true;
        cur.attrs = meta::decode_inode(v);
      } else if (k.kind == item_kind::counts) {
        cur.stored = meta::decode_counts(v);
      } else if (k.kind == item_kind::entry) {
        cur.seen.entries += meta::decode_entries(v).size();
      } else if (k.kind == item_kind::chunk) {
        ChunkRef ref = meta::decode_chunk_item(v);
        cur.seen.data_bytes += ref.size;
        if (ref.packed || flagged_.contains(ref.offset) || meta_.users(ref.offset)) {
          Shared& s = shared[ref.offset];
          if (s.users.empty()) s.first = ref;
          s.users.push_back({k.object, k.offset});
        } else {
          live[meta_.segment_of(ref.offset)] += ref.stored_size;
        }
      }
      return true;
    });
    finish();
    for (const Fix& f : counts) {
      if (f.counts == InodeCounts{}) {
        meta_.tree().erase(key(f.ino, item_kind::counts));
      } else {
        meta_.tree().put(key(f.ino, item_kind::counts), meta::encode_counts(f.counts));
      }
    }
    std::set<InodeId> want(orphans.begin(), orphans.end()), have(listed.begin(), listed.end());
    for (InodeId id : have) {
      if (!want.contains(id)) meta_.tree().erase(key(0, item_kind::orphan, id));
    }
    for (InodeId id : want) {
      if (!have.contains(id)) meta_.tree().put(key(0, item_kind::orphan, id), {});
    }
    for (std::uint64_t offset : had_refs) {
      if (!shared.contains(offset)) meta_.tree().erase(meta::refs_key_of(offset));
    }
    for (auto& [offset, s] : shared) {
      std::sort(s.users.begin(), s.users.end(), by_user);
      Refs r{static_cast<std::uint32_t>(s.users.size()), s.users};
      meta_.tree().put(meta::refs_key_of(offset), meta::encode_refs(r));
      live[meta_.segment_of(offset)] += s.first.stored_size;
    }
    // Nodes: those the tree holds now, and those it replaced (still on disk
    // until the checkpoint writes the changed ones and lets them go).
    std::uint64_t node_total = 0;
    meta_.tree().for_each_node([&](const NodeRef& ref) {
      live[meta_.segment_of(ref.offset)] += ref.stored_size;
      node_total += ref.stored_size;
    });
    totals.node_bytes = node_total;
    meta_.counters = totals;
    for (auto& [index, usage] : meta_.segments) usage.live_bytes = live.contains(index) ? live[index] : 0;
    changed_ = true;
  }

  // ---- state -----------------------------------------------------------------

  static constexpr std::size_t kKeptRecords = 4096;

  struct ChunkProblem {
    InodeId ino;
    std::uint64_t index;
    std::uint32_t size;
    std::string problem;
    bool lost;  // its data cannot be kept (not so for a chunk past the end)
  };

  Store& store_;
  const Superblock& sb_;
  const FsckOptions& options_;
  FsckReport& report_;
  Metadata& meta_;
  const std::map<std::uint64_t, std::uint64_t>& node_bytes_;
  const bool repair_;

  Current cur_;
  std::vector<Ino> inodes_;    // in inode order
  std::vector<Ent> entries_;
  std::vector<InodeId> listed_orphans_;
  std::map<std::uint64_t, Refs> refs_;      // record -> its refs item
  std::map<std::uint64_t, Shared> shared_;  // record -> the chunks that list it
  std::set<std::uint64_t> flagged_;         // records shared without a list
  std::map<std::uint64_t, std::uint64_t> live_;  // segment -> bytes of single-user records
  MetaCounters want_;
  std::size_t bad_counts_ = 0;
  std::size_t bad_items_ = 0;
  std::string first_bad_;
  std::vector<TreeKey> bad_keys_;
  std::vector<ChunkProblem> problems_;
  std::uint64_t users_offset_ = UINT64_MAX;
  std::optional<std::vector<ChunkUser>> users_;
  std::unordered_map<std::uint64_t, RecordCheck> records_;
  std::list<std::uint64_t> order_;
  RecordCheck scratch_;
  std::unordered_map<InodeId, std::string> paths_;
  std::optional<InodeId> lost_found_;
  bool changed_ = false;
  bool derived_wrong_ = false;
};

}  // namespace

bool check_tree(const TreeCheckInput& input) { return TreeCheck(input).run(); }

}  // namespace nanoer
