// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/build/build_flag.h"

namespace base {

// How many slices a worker is given to draw from. More than one lets a
// worker absorb a unit that turned out larger than its share, and a slice
// costs one atomic add, so the count buys balance without a queue.
inline constexpr usize FOR_EACH_SLICES_PER_WORKER = 4;

namespace detail {

template <typename F>
void for_each_serial(usize begin, usize end, F&& body) {
  for (usize i = begin; i < end; ++i) {
    body(i);
  }
}

}  // namespace detail

// Runs `body(i)` for every index in [begin, end), spread over `jobs`
// threads. Fewer than two jobs, or a range of one index, runs the whole
// range where it was called from: a phase too small to split pays for no
// thread, and the serial path is the one a run compares against.
//
// Indices are handed out in slices through one counter rather than divided
// into contiguous blocks up front. A block division leaves a worker holding
// a large unit running while the others sit idle, and the units here are
// whatever sizes the input happened to have, so the balance cannot be
// known in advance. It also costs one atomic add per slice.
//
// `body` runs to completion: the loop keeps handing out work even when a
// unit has decided it has nothing left to do. A caller that wants part of
// the range done says so through a flag of its own, which keeps that
// decision where the work is rather than behind a return value here.
//
// Every unit of work needs to be independent. Anything two units share is
// a race, and the counter above is the only synchronisation here.
template <typename F>
void for_each(usize begin, usize end, u32 jobs, F&& body) {
#if FPAG_BUILD_FLAG(IS_OS_ASMJS)
  // A target without threads has nothing to spread the work over, and the
  // `jobs` a caller asks for cannot make it appear.
  (void)jobs;
  detail::for_each_serial(begin, end, body);
#else
  const usize count = end - begin;
  const usize workers = std::min(static_cast<usize>(jobs), count);
  if (workers < 2) {
    detail::for_each_serial(begin, end, body);
    return;
  }

  const usize slice =
      std::max<usize>(1, count / (workers * FOR_EACH_SLICES_PER_WORKER));
  std::atomic<usize> next{begin};
  const auto drain = [&] {
    while (true) {
      const usize first = next.fetch_add(slice, std::memory_order_relaxed);
      if (first >= end) {
        return;
      }
      const usize last = std::min(first + slice, end);
      for (usize i = first; i < last; ++i) {
        body(i);
      }
    }
  };
  // The calling thread takes a share rather than only waiting, so a run
  // spends one thread fewer and the caller brings warm caches to its share.
  std::vector<std::thread> threads;
  threads.reserve(workers - 1);
  for (usize w = 1; w < workers; ++w) {
    threads.emplace_back(drain);
  }
  drain();
  for (std::thread& thread : threads) {
    thread.join();
  }
#endif
}

}  // namespace base
