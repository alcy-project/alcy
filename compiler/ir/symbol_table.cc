// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/symbol_table.h"

#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "debug/check.h"
#include "debug/dcheck.h"
#include "fpag/hash/xxh3_hasher.h"

namespace ir {

namespace {

// The slots of a shard start here and double, so a probe stays short without
// arrays that are mostly holes.
constexpr usize SHARD_MIN_SLOTS = 16;

// A shard grows when the next name would fill more than this share of it. The
// probe is over control bytes, so a miss walks a line or two; keeping the
// slots half free is a few bytes a name and the names themselves are not
// copied at all.
constexpr usize SHARD_LOAD_NUMERATOR = 1;
constexpr usize SHARD_LOAD_DENOMINATOR = 2;

// A published control byte: the name's hash in its low seven bits, and a bit
// that says the slot is not free, so no fingerprint reads as an empty slot.
constexpr u8 CONTROL_PUBLISHED = 0x80;

// Where a name's probe starts inside its shard. The shard was chosen by the
// hash's low bits, so those bits are the same for every name here; the rest of
// the hash is folded over them, which is what keeps the starts spread.
u64 bucket_of(u64 hash) {
  return hash ^ (hash >> 6);
}

u8 control_of(u64 hash) {
  return static_cast<u8>((hash >> 56) & 0x7F) | CONTROL_PUBLISHED;
}

// Whether two names are the same bytes. Comparing two string_views is a call
// into memcmp, and a probe compares a candidate whenever a fingerprint
// matches, so the bytes are read as words instead.
bool same_name(std::string_view lhs, std::string_view rhs) {
  const usize length = lhs.size();
  if (length != rhs.size()) {
    return false;
  }
  const char* const left = lhs.data();
  const char* const right = rhs.data();
  usize at = 0;
  for (; at + sizeof(u64) <= length; at += sizeof(u64)) {
    u64 left_word = 0;
    u64 right_word = 0;
    std::memcpy(&left_word, left + at, sizeof(left_word));
    std::memcpy(&right_word, right + at, sizeof(right_word));
    if (left_word != right_word) {
      return false;
    }
  }
  for (; at < length; ++at) {
    if (left[at] != right[at]) {
      return false;
    }
  }
  return true;
}

}  // namespace

usize SymbolTable::shard_of(u64 hash) {
  return static_cast<usize>(hash % NUM_SHARDS);
}

SymbolTable::SymbolTable(usize max_names)
    : shards_(NUM_SHARDS), max_names_(max_names == 0 ? MAX_NAMES : max_names) {
  // The slots before the empty name's are never handed out; they keep the
  // handle the IR defaults to pointing at the empty name.
  names_.resize(str::EMPTY_STRING_ID.offset);
  const u64 hash = hash::Xxh3Hasher64{}(std::string_view{});
  names_.emplace_back();
  DCHECK_EQ(names_.size() - 1, static_cast<usize>(str::EMPTY_STRING_ID.offset));
  Shard& shard = shards_[shard_of(hash)];
  shard.control.assign(SHARD_MIN_SLOTS, 0);
  shard.indices.assign(SHARD_MIN_SLOTS, 0);
  const usize at = bucket_of(hash) & (SHARD_MIN_SLOTS - 1);
  shard.indices[at] = str::EMPTY_STRING_ID.offset;
  shard.control[at] = control_of(hash);
  shard.size = 1;
}

std::optional<u32> SymbolTable::find(const Shard& shard,
                                     u64 hash,
                                     std::string_view name) const {
  if (shard.control.empty()) {
    return std::nullopt;
  }
  const u8 control = control_of(hash);
  const usize mask = shard.control.size() - 1;
  for (usize step = 0; step <= mask; ++step) {
    const usize at = (bucket_of(hash) + step) & mask;
    const u8 published = shard.control[at];
    if (published == 0) {
      return std::nullopt;
    }
    if (published == control && same_name(names_[shard.indices[at]], name)) {
      return shard.indices[at];
    }
  }
  return std::nullopt;
}

void SymbolTable::grow(Shard& shard) {
  const usize slots = shard.control.size() * 2;
  std::vector<u8> control(slots, 0);
  std::vector<u32> indices(slots, 0);
  const usize mask = slots - 1;
  for (usize at = 0; at < shard.control.size(); ++at) {
    const u8 published = shard.control[at];
    if (published == 0) {
      continue;
    }
    // The fingerprint is a hash's top bits, and the probe of the old array
    // only says where a name is not; the name's own hash is recovered from
    // its bytes, which is what the array is rebuilt from anyway.
    const u64 hash = hash::Xxh3Hasher64{}(names_[shard.indices[at]]);
    usize grown = bucket_of(hash) & mask;
    while (control[grown] != 0) {
      grown = (grown + 1) & mask;
    }
    indices[grown] = shard.indices[at];
    control[grown] = control_of(hash);
  }
  shard.control = std::move(control);
  shard.indices = std::move(indices);
}

std::optional<SymbolTable::Id> SymbolTable::insert(std::string_view name,
                                                   u64 hash,
                                                   usize shard_index) {
  if (count() >= max_names_) {
    return std::nullopt;
  }
  const u32 index = static_cast<u32>(names_.size());
  Shard& shard = shards_[shard_index];
  if (shard.control.empty()) {
    shard.control.assign(SHARD_MIN_SLOTS, 0);
    shard.indices.assign(SHARD_MIN_SLOTS, 0);
  } else if ((shard.size + 1) * SHARD_LOAD_DENOMINATOR >
             shard.control.size() * SHARD_LOAD_NUMERATOR) {
    grow(shard);
  }
  const usize mask = shard.control.size() - 1;
  usize at = bucket_of(hash) & mask;
  while (shard.control[at] != 0) {
    at = (at + 1) & mask;
  }
  // The index is written before the control byte that publishes it, so a
  // reader that sees a published slot sees the name behind it.
  shard.indices[at] = index;
  shard.control[at] = control_of(hash);
  ++shard.size;
  names_.push_back(name);
  return Id{index};
}

std::optional<SymbolTable::Id> SymbolTable::intern_hashed(std::string_view name,
                                                          u64 hash,
                                                          bool copy) {
  const usize shard_index = shard_of(hash);
  if (const std::optional<u32> found = find(shards_[shard_index], hash, name)) {
    return Id{*found};
  }
  if (!copy) {
    return insert(name, hash, shard_index);
  }
  owned_.emplace_back(name);
  return insert(owned_.back(), hash, shard_index);
}

std::optional<SymbolTable::Id> SymbolTable::try_intern(std::string_view name) {
  return intern_hashed(name, hash::Xxh3Hasher64{}(name), false);
}

std::optional<SymbolTable::Id> SymbolTable::intern_copied(
    std::string_view name) {
  return intern_hashed(name, hash::Xxh3Hasher64{}(name), true);
}

void SymbolTable::record(Bins& bins,
                         std::string_view name,
                         bool stable,
                         u32 key) {
  if (bins.shards.empty()) {
    bins.shards.resize(NUM_SHARDS);
  }
  const u64 hash = hash::Xxh3Hasher64{}(name);
  bins.shards[shard_of(hash)].push_back(
      Pending{hash, name, key, stable, str::INVALID_STRING_POOL_ID});
}

void SymbolTable::gather(std::vector<Bins>& bins) {
  for (usize shard_index = 0; shard_index < NUM_SHARDS; ++shard_index) {
    for (Bins& producer : bins) {
      if (shard_index >= producer.shards.size()) {
        continue;
      }
      for (Pending& pending : producer.shards[shard_index]) {
        pending.handle =
            intern_hashed(pending.name, pending.hash, !pending.stable)
                .value_or(str::INVALID_STRING_POOL_ID);
      }
    }
  }
}

std::string_view SymbolTable::get(Id id) const {
  if (id.offset >= names_.size()) {
    return {};
  }
  return names_[id.offset];
}

SymbolTable::Id SymbolTable::intern(std::string_view name) {
  const std::optional<Id> id = try_intern(name);
  CHECK_MSG(id.has_value(),
            "the name table is full; size it for the names a run interns");
  return id.value_or(str::INVALID_STRING_POOL_ID);
}

}  // namespace ir
