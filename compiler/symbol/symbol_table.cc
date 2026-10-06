// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "symbol/symbol_table.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "debug/dcheck.h"
#include "fpag/hash/xxh3_hasher.h"

namespace symbol {

usize SymbolTable::shard_of(u64 hash) {
  return static_cast<usize>(hash % NUM_SHARDS);
}

SymbolTable::SymbolTable() : shards_(NUM_SHARDS) {
  // The slots before the empty name's are never handed out; they keep the
  // handle the IR defaults to pointing at the empty name.
  names_.resize(str::EMPTY_STRING_ID.offset);
  const u64 hash = hash::Xxh3Hasher64{}(std::string_view{});
  names_.push_back(std::string_view{});
  DCHECK_EQ(names_.size() - 1, static_cast<usize>(str::EMPTY_STRING_ID.offset));
  shards_[shard_of(hash)].emplace(Key{hash, std::string_view{}},
                                  str::EMPTY_STRING_ID.offset);
}

std::optional<SymbolTable::Id> SymbolTable::try_intern(std::string_view name) {
  const u64 hash = hash::Xxh3Hasher64{}(name);
  const usize shard_index = shard_of(hash);
  const Shard& shard = shards_[shard_index];
  const auto found = shard.find(Key{hash, name});
  if (found != shard.end()) {
    return Id{found->second};
  }
  return insert(name, hash, shard_index);
}

std::optional<SymbolTable::Id> SymbolTable::intern_copied(
    std::string_view name) {
  const u64 hash = hash::Xxh3Hasher64{}(name);
  const usize shard_index = shard_of(hash);
  const Shard& shard = shards_[shard_index];
  const auto found = shard.find(Key{hash, name});
  if (found != shard.end()) {
    return Id{found->second};
  }
  owned_.emplace_back(name);
  return insert(owned_.back(), hash, shard_index);
}

std::optional<SymbolTable::Id> SymbolTable::insert(std::string_view name,
                                                   u64 hash,
                                                   usize shard_index) {
  if (names_.size() >= MAX_NAMES) {
    return std::nullopt;
  }
  const u32 index = static_cast<u32>(names_.size());
  names_.push_back(name);
  shards_[shard_index].emplace(Key{hash, name}, index);
  return Id{index};
}

std::string_view SymbolTable::get(Id id) const {
  if (id.offset >= names_.size()) {
    return {};
  }
  return names_[id.offset];
}

}  // namespace symbol
