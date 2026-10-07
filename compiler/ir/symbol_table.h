// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"

namespace ir {

// The value behind a handle, for a table that keys by it.
[[nodiscard]] constexpr u32 handle_value(str::StringPoolId id) {
  return id.offset;
}

// The names a run interns, and the handles the IR carries for them.
//
// A name is held as a view of the bytes that already spell it. A parsed name
// is a view of the memory-mapped source, and a source lives for the whole
// invocation, so `try_intern` stores what it was handed and copies nothing.
// `intern_copied` is the other half: bytes that do not outlive the call -- a
// decoded string literal, a message built while lowering -- are copied into
// the table, once per distinct name.
//
// The handle is `str::StringPoolId`, the 4-byte name currency the IR stores in
// its nodes so their layouts do not depend on the host. Here its value is an
// index into the table and not an offset into a pool, and the empty name is
// interned at the slot `str::EMPTY_STRING_ID` names, so a node whose handle
// defaults reads the empty name as it always did. A handle is an index and not
// a table position, so a table that fills can be rehashed larger without
// moving the name a caller already holds.
//
// A name's hash decides the one shard it can be in, so two lookups of
// different shards never touch the same memory. A shard is two parallel
// arrays: a control byte a probe walks, and the index of the name each one
// holds. The control byte carries a fingerprint of the name's hash, so a slot
// holding some other name is rejected without reading its bytes, and a name
// is hashed once, when its lookup starts.
class SymbolTable {
 public:
  using Id = str::StringPoolId;

  // How many names the table holds by default. A run that wants more is
  // reported rather than left to exhaust memory; a caller that has to reach
  // that report asks for fewer.
  static constexpr usize MAX_NAMES = 1ull << 24;

  // Holds at most `max_names` distinct names, the empty name included; zero
  // takes MAX_NAMES.
  explicit SymbolTable(usize max_names = 0);

  SymbolTable(const SymbolTable&) = delete;
  SymbolTable& operator=(const SymbolTable&) = delete;

  // The id for `name`, whose bytes have to outlive the table.
  std::optional<Id> try_intern(std::string_view name);

  // The id for `name`, copying it into the table's own storage when it is new.
  std::optional<Id> intern_copied(std::string_view name);

  // The id for `name`, which has to be internable: bytes that outlive the
  // table, and a table with room for one more. A name that cannot be interned
  // is a bug, as it was when the table was a pool.
  Id intern(std::string_view name);

  // The name an id stands for, or the empty name for an id this table did not
  // mint.
  std::string_view get(Id id) const;

  // How many distinct names the table holds, the empty name included. The
  // slots before the empty name's hold no name.
  usize count() const { return names_.size() - str::EMPTY_STRING_ID.offset; }

  // One name a producer wants a handle for. The producer keeps the entries it
  // recorded and reads the handle a gather left in them, so a gather needs no
  // stable slot in anybody else's table.
  struct Pending {
    // The name's hash, taken where the name was recorded, so a gather does not
    // take it again.
    u64 hash = 0;
    std::string_view name;
    // The producer's own key for the name, which is how it finds the entry
    // again once the gather has run.
    u32 key = 0;
    // Whether the name's bytes outlive the table. A name that came out of a
    // source does; a synthesized one is copied by the gather.
    bool stable = true;
    // What the gather left: the handle, or INVALID when the table is full.
    Id handle = str::INVALID_STRING_POOL_ID;
  };

  // The names one producer recorded, one run per shard. A producer appends to
  // its own bins and no other producer touches them, so recording takes no
  // lock.
  struct Bins {
    std::vector<std::vector<Pending>> shards;
  };

  // Records one name for the bins' producer.
  static void record(Bins& bins, std::string_view name, bool stable, u32 key);

  // Interns every recorded name and leaves each entry's handle in it. A shard
  // is visited before the next and the bins in the order they are given, so
  // the caller indexes them by what produced them -- a module, a file -- and
  // not by the order the work happened to finish in.
  void gather(std::vector<Bins>& bins);

 private:
  // The number of tables the names are spread over.
  static constexpr usize NUM_SHARDS = 64;

  // A shard's slots: the control byte a probe walks, and the index of the
  // name under it. A control byte is zero when the slot is free and carries a
  // fingerprint of the name's hash otherwise, so a name that is not here is
  // rejected by one byte.
  struct Shard {
    std::vector<u8> control;
    std::vector<u32> indices;
    usize size = 0;
  };

  // The id for a name whose hash is already known, copying its bytes when they
  // do not outlive the table.
  std::optional<Id> intern_hashed(std::string_view name, u64 hash, bool copy);

  // The shard a hash falls in.
  static usize shard_of(u64 hash);

  // The index of `name` in `shard`, or nothing when the shard does not hold
  // it. A free slot ends the probe: insertion always fills the first one it
  // meets, so a name cannot sit past a free slot.
  std::optional<u32> find(const Shard& shard,
                          u64 hash,
                          std::string_view name) const;

  // Puts a name that `find` did not answer into its shard, growing the shard
  // first when its slots are nearly all taken.
  std::optional<Id> insert(std::string_view name, u64 hash, usize shard_index);

  // Moves a shard's slots into arrays twice as large.
  void grow(Shard& shard);

  std::vector<std::string_view> names_;
  // The names that had to be copied, in a container that never moves what it
  // holds: a short string keeps its bytes inside the string object.
  std::deque<std::string> owned_;
  // One pair of slot arrays per shard, grown on demand.
  std::vector<Shard> shards_;
  usize max_names_ = MAX_NAMES;
};

}  // namespace ir
