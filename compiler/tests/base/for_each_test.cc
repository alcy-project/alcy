// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "base/for_each.h"

#include <atomic>
#include <chrono>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/build/build_flag.h"
#include "fpag/debug/thread_id.h"

namespace base {

namespace {

// Every index runs, whatever the job count, and none runs twice. The
// counters are atomic because a threaded run reaches them from several
// threads, and an unsynchronised increment would lose counts.
void check_each_index_runs_once(usize begin, usize end, u32 jobs) {
  std::vector<u32> visits(end - begin, 0);
  for_each(begin, end, jobs, [&](usize i, usize) { ++visits[i - begin]; });
  for (usize i = begin; i < end; ++i) {
    CHECK(visits[i - begin] == 1);
  }
}

}  // namespace

// The range is the whole of the contract: every index, once. A job count
// past the number of indices has nothing extra to spread the work over,
// and a count of one is the serial path a threaded run is compared with.
TEST_CASE("for_each runs every index once at any job count") {
  for (u32 jobs : {0u, 1u, 2u, 4u, 8u, 64u}) {
    check_each_index_runs_once(0, 37, jobs);
    check_each_index_runs_once(5, 6, jobs);
    check_each_index_runs_once(0, 1, jobs);
    check_each_index_runs_once(0, 0, jobs);
  }
}

// Work reaches more than one thread, which is the whole point of the
// counter. The unit that arrives first holds the run at a gate until a
// second unit arrives, so a loop that put the whole range on one thread
// cannot finish: it would wait out the bound and fail the case. Which
// threads those are has no bearing on the claim, only how many.
//
// Compiled only where there are threads to spread the work over. A target
// without them runs the range where the call was made from, which is the
// contract the case above already exercises at every job count - and there
// is no second thread here for the first unit to wait for, so asking for
// one would fail a build that is behaving as documented.
#if !FPAG_BUILD_FLAG(IS_OS_ASMJS)
TEST_CASE("for_each spreads work over more than one thread") {
  constexpr usize COUNT = 32;
  constexpr auto BOUND = std::chrono::seconds(2);
  std::atomic<usize> arrived{0};
  std::atomic<bool> released{false};
  std::atomic<u64> waiting_on{0};
  std::atomic<u64> released_by{0};

  for_each(0, COUNT, 4, [&](usize, usize) {
    if (arrived.fetch_add(1, std::memory_order_acq_rel) == 0) {
      waiting_on.store(debug::current_thread_id(), std::memory_order_relaxed);
      const auto until = std::chrono::steady_clock::now() + BOUND;
      while (!released.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < until) {}
      return;
    }
    // The unit that opens the gate names itself. A serial loop reaches this
    // body again only once the first unit has given up, which is too late
    // and comes from the thread that was already waiting.
    bool expected = false;
    if (released.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
      released_by.store(debug::current_thread_id(), std::memory_order_relaxed);
    }
  });

  CHECK(released.load(std::memory_order_acquire));
  CHECK(released_by.load(std::memory_order_relaxed) !=
        waiting_on.load(std::memory_order_relaxed));
}
#endif  // !FPAG_BUILD_FLAG(IS_OS_ASMJS)

}  // namespace base
