// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

#include "fpag/base/numeric.h"

namespace bench {

// The summary of one case's samples. The engine computes only what a
// reader is shown; everything else is a question about a distribution,
// and the raw samples are written so it can be asked of the file rather
// than of the harness.
struct Stats {
  u32 count = 0;
  u64 min = 0;
  // Percentiles are fractional because the rule interpolates: for an
  // even number of samples the median is the mean of the two middle
  // ones, and truncating it would make this a different statistic from
  // the one the process runner reports.
  f64 p50 = 0.0;
  f64 p95 = 0.0;
  u64 max = 0;
  f64 mean = 0.0;
  f64 stddev = 0.0;
};

// Summarizes samples in nanoseconds. `samples` is taken by value so the
// order statistics may sort it; the caller keeps the original for the
// raw record.
//
// The percentile rule is linear interpolation at `p * (n - 1)`, which
// for a median is Python's `statistics.median` and for a higher
// percentile is `statistics.quantiles(method="inclusive")`. The process
// runner uses the same rule, and a test pins this implementation to
// those, because two runners reporting `p50` must mean the same number.
Stats summarize(std::span<const u64> samples);

}  // namespace bench
