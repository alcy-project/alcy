// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"

namespace pkg {

// Copies bytes into arena storage, yielding a view that outlives the
// caller's buffer. Empty input yields an empty view rather than an
// allocation, so the common "no value" case costs nothing.
inline std::string_view copy_str(mem::Arena& arena, std::string_view bytes) {
  if (bytes.empty()) {
    return {};
  }
  char* const mem =
      static_cast<char*>(arena.alloc(bytes.size(), alignof(char)));
  for (usize i = 0; i < bytes.size(); ++i) {
    mem[i] = bytes[i];
  }
  return {mem, bytes.size()};
}

}  // namespace pkg
