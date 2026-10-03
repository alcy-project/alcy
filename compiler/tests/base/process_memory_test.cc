// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "base/process_memory.h"

#include <vector>

#include "config/build_config.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace base {

namespace {

// Touched, so every page is resident and the high-water mark has to
// include them: an untouched allocation commits nothing on most hosts.
void hold(usize bytes) {
  std::vector<char> memory(bytes, 1);
  volatile char sink = memory[bytes / 2];
  (void)sink;
}

}  // namespace

TEST_CASE("the peak memory is measured, and in bytes") {
#if BUILD_FLAG(IS_OS_ASMJS)
  // No measurement is the platform's answer, and zero is how it is
  // reported.
  CHECK(peak_memory_bytes() == 0);
#else
  constexpr usize MIB = static_cast<usize>(1024) * 1024;
  hold(64 * MIB);
  // A host that reports kibibytes into a field read as bytes answers
  // about a sixty-fourth of what the test holds, so the lower bound is
  // what pins the unit down.
  CHECK(peak_memory_bytes() >= 64 * MIB);
#endif
}

}  // namespace base
