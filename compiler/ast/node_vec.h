// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <atomic>
#include <utility>

#include "ast/lane.h"
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

  // How many appends may be in flight at once.
  //
  // More than one gives each appending thread a lane, so an append moves a
  // cursor no other thread can move and needs no atomic operation. A lane
  // cannot borrow, and the appends of a package are not spread the way its
  // room is, so the lanes hold four fifths of the reservation and the rest is
  // a pool any lane that fills reaches for: a table whose appends turned out
  // uneven fills its pool rather than refusing a file it has room for.
  //
  // One keeps this table's share in hand for the batch instead: the check and
  // the append are not one step, so what the check admits must cover every
  // append that can follow it before the arena has moved.
  void set_parallel_slots(u32 jobs) {
    if (jobs > 1) {
      lanes_ = jobs;
      const usize pool = arena_.capacity() / 5;
      reserved_ = static_cast<usize>(jobs) * sizeof(T);
      // The lane is cut on the stride and not on the alignment. A node's index
      // is its offset divided by its size, so a slice that begins part-way
      // into a node would give every node after it an index that reads the one
      // before: the division would truncate. The pool is cut the same way, and
      // it is the room several lanes may reach for at once, which is what its
      // reserve covers.
      arena_.set_lanes(jobs, sizeof(T), pool);
      return;
    }
    lanes_ = 0;
    reserved_ = 0;
  }

  // Whether `idx` addresses a node this table holds, treating an index that
  // names nothing as one that does (a child that is absent is not a fault).
  //
  // The room a table has filled is a set of runs and not one range: a lane is
  // a run, the lanes are not adjacent, and a lane that filled its slice went
  // on in the pool. Comparing against `size()` would admit the room between
  // them, which no node was written to and which the walk that follows would
  // read.
  [[nodiscard]] bool bound(const Idx idx) const noexcept {
    if (!idx.is_valid()) {
      return true;
    }
    const usize at = static_cast<usize>(idx.idx) * sizeof(T);
    for (u32 lane = 0; lane < lanes_; ++lane) {
      const usize begin = arena_.lane_begin(lane);
      if (at >= begin && at < begin + arena_.lane_size(lane)) {
        return true;
      }
    }
    return at >= arena_.pool_begin() &&
           at < arena_.pool_begin() + arena_.pool_size();
  }

  // Calls `body(Idx)` for every node the table holds, in index order within
  // each run, and for no other. This is what a walk over a table is: the room
  // between two lanes is not a node, and `size()` counts it.
  template <typename F>
  void for_each_node(F&& body) const {
    for (u32 lane = 0; lane < lanes_; ++lane) {
      const usize first = arena_.lane_begin(lane) / sizeof(T);
      const usize count = first + arena_.lane_size(lane) / sizeof(T);
      for (usize i = first; i < count; ++i) {
        body(Idx(static_cast<IdxType>(i)));
      }
    }
    const usize first = arena_.pool_begin() / sizeof(T);
    const usize count = first + arena_.pool_size() / sizeof(T);
    for (usize i = first; i < count; ++i) {
      body(Idx(static_cast<IdxType>(i)));
    }
  }

  // Appends a node, answering the index that addresses it, or nothing when
  // the reservation cannot hold it. Exhaustion is a property of the input:
  // a caller that sees an invalid index and `exhausted` set refuses the
  // file rather than reading a tree that was never finished. Nothing is
  // written in that case, so the count still says how many nodes the table
  // holds.
  template <typename... Args>
  Idx emplace_back(Args&&... args) {
    void* mem = nullptr;
    if (lanes_ > 0) {
      const u32 lane = ast::current_lane();
      FPAG_DCHECK_LT(lane, lanes_);
      mem = arena_.alloc_from(lane, sizeof(T), alignof(T));
    }
    if (mem == nullptr) {
      // The room a table has for the case its lanes are uneven, and the room a
      // table with no lanes has for the batch. The arena traps when asked for
      // more than it reserved, so this is checked before the request: one
      // append's size plus whatever the concurrent appends still need.
      const usize used = arena_.size();
      if (used + sizeof(T) + reserved_ > arena_.capacity()) {
        exhausted_.store(true, std::memory_order_relaxed);
        return Idx::invalid();
      }
      mem = arena_.alloc_exact(sizeof(T), alignof(T));
    }
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

  // How far the table's nodes reach, as an index. Between two lanes there is
  // room no node was written to, so this is an upper bound on the nodes the
  // table holds and not a count of them: a walk goes through for_each_node and
  // a validity check through bound.
  [[nodiscard]] usize size() const noexcept {
    usize end = arena_.pool_begin() + arena_.pool_size();
    for (u32 lane = 0; lane < lanes_; ++lane) {
      const usize reach = arena_.lane_begin(lane) + arena_.lane_size(lane);
      end = reach > end ? reach : end;
    }
    return end / sizeof(T);
  }

  // How much of the reservation is left, against how much must stay in hand
  // for the input a caller is about to read.
  [[nodiscard]] bool nearly_full() const {
    if (lanes_ == 0) {
      return reservation_nearly_full(arena_.capacity(), arena_.size());
    }
    // A lane that is nearly full is not a table that is nearly full: what the
    // lane cannot hold the pool holds, and an append that finds its lane short
    // goes there. So the room left for the next file is the pool's, and a
    // nearly full pool is the whole answer.
    return reservation_nearly_full(arena_.capacity(),
                                   arena_.pool_begin() + arena_.pool_size());
  }

  // True once an append found the reservation short. The tree built so far
  // is incomplete, so a caller stops rather than reading it.
  [[nodiscard]] bool exhausted() const {
    return exhausted_.load(std::memory_order_relaxed);
  }

 private:
  [[nodiscard]] char* base() const noexcept {
    // The arena's base is fixed for its lifetime and fpag exposes it as
    // const; the table writes through it.
    // ast-grep-ignore: no-const-cast
    return const_cast<char*>(arena_.base_ptr());
  }

  [[nodiscard]] usize offset(IdxType idx) const noexcept {
    return static_cast<usize>(idx) * sizeof(T);
  }

  mem::ConcurrentArena arena_;
  // Bytes held back for appends that are in flight while this one is
  // checked; zero when one parser appends at a time.
  usize reserved_ = 0;
  u32 lanes_ = 0;
  std::atomic<bool> exhausted_{false};
};

}  // namespace ast
