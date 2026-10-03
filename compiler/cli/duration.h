// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>

#include "fpag/base/numeric.h"

namespace cli {

// A duration as text: the largest unit that keeps three significant
// digits - `4.62 us`, `337 us`, `57.9 ms`, `1.02 s` - so a column of
// them scans without the decimal point wandering. Nanoseconds stay
// nanoseconds rather than rounding down to `0.00 us`. The units are
// ASCII, which is what keeps the output printable on a terminal that is
// not UTF-8. One definition, so the result note and the trace rows
// agree about a duration by construction.
std::string format_duration(u64 ns);

}  // namespace cli
