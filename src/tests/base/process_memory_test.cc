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
// include them: an untouched allocation commits nothing on most hosts,
// and an optimizing build drops stores whose bytes are never read.
#if !BUILD_FLAG(IS_OS_ASMJS)
void hold(usize bytes) {
  std::vector<char> memory(bytes, 1);
  // One read per page through a volatile sink keeps the fill alive and
  // makes each page resident; 4 KiB is at or below any page size here.
  volatile char sink = 0;
  for (usize i = 0; i < bytes; i += 4096) {
    sink = static_cast<char>(sink ^ memory[i]);
  }
  sink = static_cast<char>(sink ^ memory[bytes - 1]);
  (void)sink;
}
#endif

}  // namespace

TEST_CASE("the peak memory is measured, and in bytes") {
#if BUILD_FLAG(IS_OS_ASMJS)
  // No measurement is the platform's answer, and zero is how it is
  // reported.
  CHECK(peak_memory_bytes() == 0);
#else
  constexpr usize MIB = static_cast<usize>(1024) * 1024;
  hold(64 * MIB);
  // The bound is what pins the unit down: a host that reports kibibytes
  // into a field read as bytes answers about a thousandth of what the
  // test holds. It stays far below the held figure on purpose - what is
  // checked is the unit of the host's account, not the allocator's.
  CHECK(peak_memory_bytes() >= MIB);
#endif
}

}  // namespace base
