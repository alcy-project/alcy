// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/clock.h"

#include <chrono>

#include "fpag/base/numeric.h"

namespace bench {

u64 resolution_ns() noexcept {
  // A clock whose period is not one nanosecond would report counts in
  // its own units; the conversion is exact for the second case and
  // rounds for anything coarser, which no supported platform has.
  const auto period = static_cast<u64>(std::chrono::steady_clock::period::num);
  return period == 0 ? 1 : period;
}

u64 measure_overhead_ns() noexcept {
  // Back-to-back reads with nothing between them, so what is left is the
  // cost of asking. Averaged over enough reads to be stable: one pair
  // is a few tens of nanoseconds, which the scheduler alone can move.
  constexpr usize READS = 10000;
  SteadyClock clock;
  const u64 started = clock.now_ns();
  u64 sink = 0;
  for (usize i = 0; i < READS; ++i) {
    sink += clock.now_ns();
  }
  const u64 elapsed = clock.now_ns() - started;
  // The reads are a use; a benchmark binary that optimized them away
  // would report an overhead of zero and believe it.
  if (sink == 0) {
    return 0;
  }
  return elapsed / READS;
}

}  // namespace bench
