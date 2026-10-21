// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/duration.h"

#include <string>

#include "fmt/format.h"
#include "fpag/base/numeric.h"

namespace cli {

namespace {

// Fixed decimals per decade, so every figure in a unit carries three
// significant digits and the point never moves within the column.
std::string scaled(f64 value) {
  if (value >= 100.0) {
    return fmt::format("{:.0f}", value);
  }
  if (value >= 10.0) {
    return fmt::format("{:.1f}", value);
  }
  return fmt::format("{:.2f}", value);
}

}  // namespace

std::string format_duration(u64 ns) {
  constexpr u64 NANOS_PER_MICRO = 1000;
  constexpr u64 NANOS_PER_MILLI = 1000 * NANOS_PER_MICRO;
  constexpr u64 NANOS_PER_SECOND = 1000 * NANOS_PER_MILLI;
  if (ns < NANOS_PER_MICRO) {
    return fmt::format("{} ns", ns);
  }
  if (ns < NANOS_PER_MILLI) {
    return scaled(static_cast<f64>(ns) / static_cast<f64>(NANOS_PER_MICRO)) +
           " us";
  }
  if (ns < NANOS_PER_SECOND) {
    return scaled(static_cast<f64>(ns) / static_cast<f64>(NANOS_PER_MILLI)) +
           " ms";
  }
  return scaled(static_cast<f64>(ns) / static_cast<f64>(NANOS_PER_SECOND)) +
         " s";
}

}  // namespace cli
