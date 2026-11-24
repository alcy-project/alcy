// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "base/process_memory.h"

#include "config/build_config.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/page_allocator.h"

#if BUILD_FLAG(IS_OS_WIN)
// windows.h first, before the SDK headers that lean on its types.
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#elif !BUILD_FLAG(IS_OS_ASMJS)
#include <sys/resource.h>

#include <fstream>
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

u64 current_memory_bytes() {
#if BUILD_FLAG(IS_OS_WIN)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ==
      0) {
    return 0;
  }
  return static_cast<u64>(counters.WorkingSetSize);
#elif BUILD_FLAG(IS_OS_ASMJS)
  return 0;
#elif BUILD_FLAG(IS_OS_APPLE)
  return 0;
#else
  // /proc/self/statm reads two integers where the loader wrote a whole
  // line: the resident count is the second field, in pages of the size
  // getconf PAGESIZE names.
  std::ifstream statm("/proc/self/statm");
  usize total = 0;
  usize resident = 0;
  if (!(statm >> total >> resident)) {
    return 0;
  }
  return resident * mem::page_size();
#endif
}

}  // namespace base
