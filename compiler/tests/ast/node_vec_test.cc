// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/node_vec.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "ast/ast.h"
#include "base/for_each.h"
#include "doctest/doctest.h"
#include "fpag/mem/page_allocator.h"

namespace ast {

namespace {

// A region big enough for the tables a case builds and small enough that a
// case can spend it. The shares are whole parts, so one part of a mebibyte
// is a mebibyte's thousandth rounded down to a page.
constexpr usize REGION_BYTES = 4u << 20;

}  // namespace

// Appending from several threads hands out every index once, and each index
// addresses the node that was appended for it. The claims are what a race
// would corrupt: two nodes sharing one index, or a hole that leaves an index
// addressing a node that was never built.
TEST_CASE("A node table takes one index per node under concurrent appends") {
  constexpr usize COUNT = 4096;
  constexpr u32 THREADS = 8;
  NodeVec<u64, base::Idx<u64, u32>> table{REGION_BYTES};

  base::for_each(0, THREADS, THREADS, [&](usize t) {
    for (usize i = t; i < COUNT; i += THREADS) {
      (void)table.emplace_back(i);
    }
  });

  CHECK(table.size() == COUNT);
  std::vector<bool> seen(COUNT, false);
  for (usize i = 0; i < COUNT; ++i) {
    const u64 value = table[i];
    CHECK(value < COUNT);
    // A claim handed out twice would show the same value at two indices.
    CHECK(!seen[value]);
    seen[value] = true;
  }
  // Two claims landing on one index lose a node as well as repeating one,
  // and the count alone does not show it: the append still publishes. A
  // value that was overwritten is never seen at all.
  for (usize v = 0; v < COUNT; ++v) {
    CHECK(seen[v]);
  }
}

// Reading an index addresses its own node: the offset is the index times the
// node size, with nothing in between to shift it.
TEST_CASE("A node table addresses each node by its own index") {
  NodeVec<ExprNode, ExprIdx> table{REGION_BYTES};

  for (u32 i = 0; i < 16; ++i) {
    ExprNode node{};
    node.kind = static_cast<ast::ExprKind>(i);
    (void)table.emplace_back(node);
  }

  CHECK(table.size() == 16);
  for (u32 i = 0; i < 16; ++i) {
    CHECK(static_cast<u32>(table[ExprIdx{i}].kind) == i);
    CHECK(static_cast<u32>(table[i].kind) == i);
  }
  CHECK(table.begin() == table.data());
  CHECK(table.end() == table.data() + table.size());
}

// An append the reservation cannot hold is refused and recorded, which is
// what lets a parser stop where the arena underneath would trap.
TEST_CASE("A node table refuses an append it cannot hold") {
  NodeVec<u64, base::Idx<u64, u32>> table{mem::page_size()};
  const usize room = mem::page_size() / sizeof(u64);
  for (usize i = 0; i < room; ++i) {
    CHECK(table.emplace_back(i).is_valid());
  }
  CHECK(!table.exhausted());
  CHECK(!table.emplace_back(room).is_valid());
  CHECK(table.exhausted());
  // Everything written before the refusal is still there.
  CHECK(table.size() == room);
  CHECK(table[0] == 0);
  CHECK(table[room - 1] == room - 1);
}

// With appends in flight, each admitted append keeps the whole batch's
// room in hand, so the table stops short of its end rather than past it.
TEST_CASE("A node table leaves room for the appends that race with it") {
  NodeVec<u64, base::Idx<u64, u32>> table{mem::page_size()};
  table.set_parallel_slots(4);
  const usize room = mem::page_size() / sizeof(u64);
  usize accepted = 0;
  while (table.emplace_back(accepted).is_valid()) {
    ++accepted;
  }
  CHECK(table.exhausted());
  CHECK(accepted + 4 <= room);
  CHECK(accepted > room - 8);
}

// The same rule under real threads: every append that was taken fits, and
// the count is the number of accepted appends - no claim lost, none
// granted twice.
TEST_CASE("A node table does not overrun under a racing batch") {
  NodeVec<u64, base::Idx<u64, u32>> table{REGION_BYTES};
  table.set_parallel_slots(4);
  std::atomic<usize> accepted{0};
  base::for_each(0, 4, 4, [&](usize t) {
    for (;;) {
      if (!table.emplace_back(static_cast<u64>(t)).is_valid()) {
        break;
      }
      accepted.fetch_add(1, std::memory_order_relaxed);
    }
  });
  CHECK(table.exhausted());
  CHECK(table.size() == accepted.load());
  CHECK(table.size() <= REGION_BYTES / sizeof(u64));
}

// The answer a caller asks before reading another file turns on what is
// left of the reservation rather than on how much input it has read.
TEST_CASE("A node table reports its reservation nearly spent") {
  NodeVec<u64, base::Idx<u64, u32>> table{REGION_BYTES};
  CHECK(!table.nearly_full());

  const usize room = REGION_BYTES / sizeof(u64);
  while (!table.nearly_full() && table.size() < room) {
    (void)table.emplace_back(static_cast<u64>(table.size()));
  }
  CHECK(table.nearly_full());
  // It ran out of headroom with the table still short of its reservation,
  // which is the room left for the file being read.
  CHECK(table.size() < room);
  CHECK(table.size() > room - room / ast::RESERVATION_HEADROOM);
}

}  // namespace ast
