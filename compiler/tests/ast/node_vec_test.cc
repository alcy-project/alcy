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
