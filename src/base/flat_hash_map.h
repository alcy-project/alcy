// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "fpag/base/numeric.h"

namespace base {

// An open-addressed table from a packed `u64` key to a value. Entries live in
// the table itself, so an insert allocates only when the table grows; a
// node-based map spends an allocation per key, and a package keys these
// tables once per expression it checks.
template <typename Value>
class FlatHashMap {
 public:
  // Smallest table, in slots. A module keys a few hundred entries, so a
  // table that starts this small reaches its size in a few doublings.
  static constexpr usize MIN_SLOTS = 8;

  [[nodiscard]] const Value* find(u64 key) const {
    if (slots_.empty()) {
      return nullptr;
    }
    const usize mask = slots_.size() - 1;
    for (usize i = home(key) & mask;; i = (i + 1) & mask) {
      const Slot& slot = slots_[i];
      if (!slot.used) {
        return nullptr;
      }
      if (slot.key == key) {
        return &slot.value;
      }
    }
  }

  // Records `value` under `key`, replacing a value already there.
  void set(u64 key, Value value) {
    if (slots_.empty()) {
      grow(MIN_SLOTS);
    }
    Slot* slot = probe(key);
    if (slot->used) {
      slot->value = value;
      return;
    }
    if ((size_ + 1) * 4 > slots_.size() * 3) {
      grow(slots_.size() * 2);
      slot = probe(key);
    }
    slot->key = key;
    slot->value = value;
    slot->used = true;
    ++size_;
  }

  [[nodiscard]] usize size() const { return size_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }

 private:
  struct Slot {
    u64 key = 0;
    Value value{};
    bool used = false;
  };

  // Fibonacci mixing: the keys are packed indices, so neighbouring keys
  // differ only in their low bits.
  [[nodiscard]] static usize home(u64 key) {
    return static_cast<usize>((key * 0x9E3779B97F4A7C15ull) >> 32);
  }

  // The slot holding `key`, or the empty slot it would land in. Reaching the
  // empty slot needs one to exist, which the load factor keeps.
  [[nodiscard]] Slot* probe(u64 key) {
    const usize mask = slots_.size() - 1;
    usize i = home(key) & mask;
    while (slots_[i].used && slots_[i].key != key) {
      i = (i + 1) & mask;
    }
    return &slots_[i];
  }

  void grow(usize slots) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.resize(slots);
    for (const Slot& slot : old) {
      if (!slot.used) {
        continue;
      }
      Slot& moved = *probe(slot.key);
      moved.key = slot.key;
      moved.value = slot.value;
      moved.used = true;
    }
  }

  std::vector<Slot> slots_;
  usize size_ = 0;
};

}  // namespace base
