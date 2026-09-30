// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <chrono>

#include "fpag/base/numeric.h"

namespace bench {

// The measurement clock: a policy with one method, so a test can supply
// a deterministic one and a different clock replaces one file. It is
// deliberately a template policy rather than a direct call, because the
// measurement loop must inline it and because a fake clock is what makes
// a duration testable without a platform in the loop.
//
// Monotonic and standard-library only. A dedicated platform clock would
// buy immunity to clock slewing, which a run measured in hundreds of
// milliseconds does not meet, and its call cost is the same, so batching
// is required either way. See docs/adr/0021-benchmark-measurement.md.
struct SteadyClock {
  u64 now_ns() noexcept {
    return static_cast<u64>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }
};

// Granularity of the clock in nanoseconds. Traveled with every result,
// because a sample smaller than this says nothing about the work.
u64 resolution_ns() noexcept;

// Cost of reading the clock, measured by timing back-to-back reads.
// Traveled with every result so a reader can tell how much of a short
// sample is the measurement rather than the thing measured.
u64 measure_overhead_ns() noexcept;

}  // namespace bench
