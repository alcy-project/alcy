// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "base/process_memory.h"

#include "config/build_config.h"
#include "fpag/base/numeric.h"

#if BUILD_FLAG(IS_OS_WIN)
// windows.h first, before the SDK headers that lean on its types.
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#elif !BUILD_FLAG(IS_OS_ASMJS)
#include <sys/resource.h>
#endif

namespace base {

u64 peak_memory_bytes() {
#if BUILD_FLAG(IS_OS_WIN)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ==
      0) {
    return 0;
  }
  return static_cast<u64>(counters.PeakWorkingSetSize);
#elif BUILD_FLAG(IS_OS_ASMJS)
  return 0;
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0;
  }
  // macOS reports the high-water mark in bytes; Linux and the other
  // POSIX hosts report it in kibibytes.
#if BUILD_FLAG(IS_OS_APPLE)
  return static_cast<u64>(usage.ru_maxrss);
#else
  return static_cast<u64>(usage.ru_maxrss) * 1024;
#endif
#endif
}

}  // namespace base
