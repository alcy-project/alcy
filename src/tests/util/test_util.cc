// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tests/util/test_util.h"

#include <string_view>

#include "config/build_config.h"
#include "fpag/debug/logger.h"
#include "fpag/logging/sink/stdout_sink.h"
#include "fpag/term/console.h"

namespace tests {

void init_logger() {
  debug::init_debug_logger();
}

bool is_object_bytes(std::string_view bytes) {
#if BUILD_FLAG(IS_OS_WIN)
  if (bytes.size() < 2) {
    return false;
  }
  const std::string_view machine = bytes.substr(0, 2);
  return machine == "\x64\x86" || machine == "\x4C\x01" ||
         machine == "\x64\xAA";
#elif BUILD_FLAG(IS_OS_APPLE)
  return bytes.size() >= 4 && bytes.compare(0, 4, "\xCF\xFA\xED\xFE") == 0;
#else
  return bytes.size() >= 4 && bytes.compare(0, 4,
                                            "\x7F"
                                            "ELF") == 0;
#endif
}

}  // namespace tests
