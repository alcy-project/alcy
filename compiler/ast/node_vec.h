// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <atomic>
#include <utility>

#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/check.h"
#include "fpag/mem/concurrent_arena.h"
#include "fpag/mem/page_allocator.h"

namespace ast {

// The fraction of a reservation a read leaves in hand, so the file being
// read has room to spend.
inline constexpr usize RESERVATION_HEADROOM = 8;

// Whether a reservation with `used` of `capacity` spent is close enough to
// full that what comes next has nowhere to go. Measured rather than
// predicted: what an input costs is the identifiers it names, not how many
// bytes it has.
[[nodiscard]] inline bool reservation_nearly_full(usize capacity, usize used) {
  return capacity - used < capacity / RESERVATION_HEADROOM;
}

// One table of AST nodes, appended to by a parser and read by every pass
// after it.
//
// A table is a reservation and a bump rather than a growing array, which is
// what lets several parsers append to one table at once: the reservation is
// taken once, before any of them starts, and an append is one atomic add
// inside it. `operator[]` stays a load off the table's base, with no
// indirection to add - which matters, because reading a node table is what
// the passes that dominate a run spend their time on.
//
// An index is the place in the reservation a node was built at, which is what
// makes `operator[]` arithmetic on it. Two threads can therefore append out of
// order and still be read afterwards: what a table holds is a set of addresses,
// and the order they were taken in is nobody's business once the appends are
// done.
//
// The reservation is a limit rather than a starting size. A table that fills
// reports it: `AstArena::nearly_full` is what a caller asks before reading
// another file, and the answer is a diagnostic naming the input.
template <typename T, base::HasIdxType Idx>
class NodeVec {
 public:
  using value_type = T;
  using IdxType = typename Idx::IdxType;

  NodeVec() = default;
  explicit NodeVec(usize capacity_bytes) {
    // A reservation is taken in whole pages, so a figure that is not a
    // multiple of one gives its remainder to nobody.
    arena_.reserve(capacity_bytes - capacity_bytes % mem::page_size());
  }

  NodeVec(const NodeVec&) = delete;
  NodeVec& operator=(const NodeVec&) = delete;
  NodeVec(NodeVec&&) noexcept = default;
  NodeVec& operator=(NodeVec&&) noexcept = default;

  // How many appends may be in flight at once. More than one keeps this
  // table's share of the reservation in hand for the batch: the check and
  // the append are not one step, so what the check admits must cover every
  // append that can follow it before the arena has moved. The arena takes the
  // room in one operation and checks it against the end, so this reserve is
  // what keeps a batch of appends inside the reservation.
  void set_parallel_slots(u32 jobs) {
    reserved_ = (jobs > 1 ? static_cast<usize>(jobs) : 0) * sizeof(T);
  }

  // Appends a node, answering the index that addresses it, or nothing when
  // the reservation cannot hold it. Exhaustion is a property of the input:
  // a caller that sees an invalid index and `exhausted` set refuses the
  // file rather than reading a tree that was never finished. Nothing is
  // written in that case, so the count still says how many nodes the table
  // holds.
  template <typename... Args>
  Idx emplace_back(Args&&... args) {
    // The arena traps when asked for more than it reserved, so the room is
    // checked here, before the request: one append's size plus whatever the
    // concurrent appends may still need.
    const usize used = arena_.size();
    if (used + sizeof(T) + reserved_ > arena_.capacity()) {
      exhausted_.store(true, std::memory_order_relaxed);
      return Idx::invalid();
    }
    void* const mem = arena_.alloc_exact(sizeof(T), alignof(T));
    if (mem == nullptr) [[unlikely]] {
      exhausted_.store(true, std::memory_order_relaxed);
      return Idx::invalid();
    }
    new (mem) T(std::forward<Args>(args)...);
    // Every allocation takes exactly sizeof(T) - a whole number of the node's
    // own alignment, so the bump never rounds - which is what makes the
    // distance from the base the index.
    return Idx(static_cast<IdxType>(
        (static_cast<char*>(mem) - arena_.base_ptr()) / sizeof(T)));
  }

  Idx push_back(const T& node) { return emplace_back(node); }
  Idx push_back(T&& node) { return emplace_back(std::move(node)); }

  [[nodiscard]] const T& operator[](const Idx idx) const {
    FPAG_DCHECK_LT(static_cast<usize>(idx.idx), size());
    return *reinterpret_cast<const T*>(base() + offset(idx.idx));
  }

  [[nodiscard]] T& operator[](const Idx idx) {
    FPAG_DCHECK_LT(static_cast<usize>(idx.idx), size());
    return *reinterpret_cast<T*>(base() + offset(idx.idx));
  }

  [[nodiscard]] const T& operator[](usize at) const {
    FPAG_DCHECK_LT(at, size());
    return *reinterpret_cast<const T*>(base() + at * sizeof(T));
  }

  [[nodiscard]] T& operator[](usize at) {
    FPAG_DCHECK_LT(at, size());
    return *reinterpret_cast<T*>(base() + at * sizeof(T));
  }

  [[nodiscard]] T* begin() noexcept { return data(); }
  [[nodiscard]] T* end() noexcept { return data() + size(); }
  [[nodiscard]] const T* begin() const noexcept { return data(); }
  [[nodiscard]] const T* end() const noexcept { return data() + size(); }

  [[nodiscard]] T* data() noexcept { return reinterpret_cast<T*>(base()); }
  [[nodiscard]] const T* data() const noexcept {
    return reinterpret_cast<const T*>(base());
  }

  [[nodiscard]] usize size() const noexcept {
    return arena_.size() / sizeof(T);
  }

  // How much of the reservation is left, against how much must stay in hand
  // for the input a caller is about to read.
  [[nodiscard]] bool nearly_full() const {
    return reservation_nearly_full(arena_.capacity(), arena_.size());
  }

  // True once an append found the reservation short. The tree built so far
  // is incomplete, so a caller stops rather than reading it.
  [[nodiscard]] bool exhausted() const {
    return exhausted_.load(std::memory_order_relaxed);
  }

 private:
  [[nodiscard]] char* base() const noexcept {
    return const_cast<char*>(arena_.base_ptr());
  }

  [[nodiscard]] usize offset(IdxType idx) const noexcept {
    return static_cast<usize>(idx) * sizeof(T);
  }

  mem::ConcurrentArena arena_;
  // Bytes held back for appends that are in flight while this one is
  // checked; zero when one parser appends at a time.
  usize reserved_ = 0;
  std::atomic<bool> exhausted_{false};
};

}  // namespace ast
