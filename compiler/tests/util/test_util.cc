// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tests/util/test_util.h"

#include <string_view>

#include "config/build_config.h"
#include "fpag/debug/logger.h"
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
#elif BUILD_FLAG(IS_OS_ASMJS)
  // The WebAssembly backend emits a wasm object, which is a module: the
  // same four magic bytes every .wasm file starts with. Falling through
  // to the ELF test below would fail on a correctly emitted object.
  //
  // The length is given explicitly because the magic begins with a NUL,
  // and the `const char*` overload of compare measures its argument with
  // strlen, which would stop at the first byte and compare nothing.
  static constexpr std::string_view WASM_MAGIC(
      "\x00"
      "asm",
      4);
  return bytes.size() >= 4 && bytes.compare(0, 4, WASM_MAGIC) == 0;
#else
  return bytes.size() >= 4 && bytes.compare(0, 4,
                                            "\x7F"
                                            "ELF") == 0;
#endif
}

bool is_bitcode_bytes(std::string_view bytes) {
  return bytes.size() >= 4 && bytes.compare(0, 4, "BC\xC0\xDE") == 0;
}

}  // namespace tests
