// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/statistics.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "fpag/base/numeric.h"

namespace bench {

namespace {

// Linear interpolation at p * (n - 1) over sorted samples. The bracket
// is clamped so an empty or single-sample set answers rather than
// indexing past the end.
f64 percentile_sorted(const std::vector<u64>& sorted, f64 p) {
  if (sorted.empty()) {
    return 0.0;
  }
  if (sorted.size() == 1) {
    return static_cast<f64>(sorted[0]);
  }
  const f64 position = p * static_cast<f64>(sorted.size() - 1);
  const usize low = static_cast<usize>(position);
  const usize high = std::min(low + 1, sorted.size() - 1);
  const f64 fraction = position - static_cast<f64>(low);
  return static_cast<f64>(sorted[low]) +
         fraction * static_cast<f64>(sorted[high] - sorted[low]);
}

}  // namespace

Stats summarize(std::span<const u64> samples) {
  Stats stats;
  stats.count = static_cast<u32>(samples.size());
  if (samples.empty()) {
    return stats;
  }
  std::vector<u64> sorted(samples.begin(), samples.end());
  std::sort(sorted.begin(), sorted.end());

  stats.min = sorted.front();
  stats.max = sorted.back();
  stats.p50 = percentile_sorted(sorted, 0.5);
  stats.p95 = percentile_sorted(sorted, 0.95);
  f64 total = 0.0;
  for (const u64 sample : sorted) {
    total += static_cast<f64>(sample);
  }
  stats.mean = total / static_cast<f64>(sorted.size());

  // Population standard deviation: the samples are the whole run, not
  // a draw from a larger one, and a single case never has enough of
  // them for the sample correction to be worth the surprise.
  f64 variance = 0.0;
  for (const u64 sample : sorted) {
    const f64 deviation = static_cast<f64>(sample) - stats.mean;
    variance += deviation * deviation;
  }
  stats.stddev = std::sqrt(variance / static_cast<f64>(sorted.size()));
  return stats;
}

}  // namespace bench
