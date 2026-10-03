// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <atomic>
#include <cstddef>
#include <utility>

#include "fpag/base/numeric.h"
#include "fpag/mem/concurrent_arena.h"
#include "fpag/mem/page_allocator.h"

namespace ast {

// The byte area identifier and path spellings are copied into, reserved
// once and appended to by every parser.
//
// The arena underneath traps when asked for more than it reserved, so
// every request is checked here first: a request is admitted only when
// the reservation can also hold the appends that may race with it.
// A request admitted with room for `slots + 1` of its own size cannot be
// overrun by any set of at most `slots` racing requests, because each of
// them leaves the same room for the others.
//
// Exhaustion is reported by returning nothing and setting `exhausted`:
// the input asked for more than the compiler reserves for syntax, which
// is a diagnostic rather than a failure.
class SpanArena {
 public:
  SpanArena() = default;
  explicit SpanArena(usize capacity_bytes) { reserve(capacity_bytes); }

  SpanArena(const SpanArena&) = delete;
  SpanArena& operator=(const SpanArena&) = delete;
  SpanArena(SpanArena&&) = delete;
  SpanArena& operator=(SpanArena&&) = delete;

  // A reservation is taken in whole pages, so a figure that is not a
  // multiple of one gives its remainder to nobody.
  void reserve(usize capacity_bytes) {
    arena_.reserve(capacity_bytes - capacity_bytes % mem::page_size());
  }

  // How many parsers may append at once. One keeps the check exact; more
  // leaves a batch's worth in hand for the race between check and append.
  void set_parallel_slots(u32 jobs) { slots_ = jobs > 1 ? jobs : 0; }

  // Appends `size` bytes aligned to `align`, or nothing when the
  // reservation cannot hold the request and its racing neighbours.
  [[nodiscard]] void* alloc(usize size,
                            usize align = alignof(std::max_align_t)) {
    const usize slack = size + align;
    // One parser at a time needs its own room and nothing more; several
    // leave the batch's room in hand, since the check and the append are
    // not one step. The division comes first so the product cannot wrap,
    // whatever the input asked for or however many threads were named.
    const usize slots = static_cast<usize>(slots_) + 1;
    if (slack > arena_.capacity() / slots ||
        arena_.size() + slack * slots > arena_.capacity()) {
      exhausted_.store(true, std::memory_order_relaxed);
      return nullptr;
    }
    void* const mem = arena_.alloc(size, align);
    if (mem == nullptr) [[unlikely]] {
      exhausted_.store(true, std::memory_order_relaxed);
    }
    return mem;
  }

  // Constructs in the arena, or returns nothing when it cannot hold the
  // object.
  template <typename T, typename... Args>
  [[nodiscard]] T* create(Args&&... args) {
    void* const mem = alloc(sizeof(T), alignof(T));
    if (mem == nullptr) [[unlikely]] {
      return nullptr;
    }
    return new (mem) T(std::forward<Args>(args)...);
  }

  [[nodiscard]] bool exhausted() const {
    return exhausted_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] const char* base_ptr() const { return arena_.base_ptr(); }
  [[nodiscard]] usize capacity() const { return arena_.capacity(); }
  [[nodiscard]] usize size() const { return arena_.size(); }

 private:
  mem::ConcurrentArena arena_;
  u32 slots_ = 0;
  std::atomic<bool> exhausted_{false};
};

}  // namespace ast
