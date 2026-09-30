// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/runner.h"

#include "fpag/base/numeric.h"

namespace bench {

namespace {

// Below this, a percentile is a claim about the scheduler. Thirty is not
// a statistical result either; it is the point past which the numbers
// stop being decided by which sample happened to be slow.
constexpr u32 MINIMUM_SAMPLES = 30;

}  // namespace

Confidence classify(u32 samples) noexcept {
  return samples < MINIMUM_SAMPLES ? Confidence::Low : Confidence::Ok;
}

}  // namespace bench
