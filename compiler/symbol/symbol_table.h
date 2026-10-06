// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"

namespace symbol {

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
// defaults reads the empty name as it always did.
//
// A name's hash decides the one table it can be in, so two lookups of
// different shards never touch the same memory. Each table keeps the hash it
// was given as part of its key, so a lookup hashes a name once.
class SymbolTable {
 public:
  using Id = str::StringPoolId;

  // How many names the table holds. A run that wants more is reported rather
  // than left to exhaust memory.
  static constexpr usize MAX_NAMES = 1ull << 24;

  SymbolTable();
  ~SymbolTable() = default;

  SymbolTable(const SymbolTable&) = delete;
  SymbolTable& operator=(const SymbolTable&) = delete;

  // The id for `name`, whose bytes have to outlive the table.
  std::optional<Id> try_intern(std::string_view name);

  // The id for `name`, copying it into the table's own storage when it is new.
  std::optional<Id> intern_copied(std::string_view name);

  // The name an id stands for, or the empty name for an id this table did not
  // mint.
  std::string_view get(Id id) const;

  // How many distinct names the table holds, the empty name included. The
  // slots before the empty name's hold no name.
  usize count() const {
    return names_.size() - str::EMPTY_STRING_ID.offset;
  }

 private:
  // The number of tables the names are spread over.
  static constexpr usize NUM_SHARDS = 64;

  // A name as a shard's table sees it: the hash it was recorded under, and the
  // bytes it spells.
  struct Key {
    u64 hash = 0;
    std::string_view name;
  };
  struct KeyHash {
    usize operator()(const Key& key) const {
      return static_cast<usize>(key.hash);
    }
  };
  struct KeyEqual {
    bool operator()(const Key& lhs, const Key& rhs) const {
      return lhs.hash == rhs.hash && lhs.name == rhs.name;
    }
  };
  using Shard = std::unordered_map<Key, u32, KeyHash, KeyEqual>;

  // The shard a hash falls in.
  static usize shard_of(u64 hash);

  // The id for a name that is not interned yet, taking a slot for it.
  std::optional<Id> insert(std::string_view name, u64 hash, usize shard_index);

  std::vector<std::string_view> names_;
  // The names that had to be copied, in a container that never moves what it
  // holds: a short string keeps its bytes inside the string object.
  std::deque<std::string> owned_;
  std::vector<Shard> shards_;
};

}  // namespace symbol
