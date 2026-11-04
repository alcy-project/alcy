// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstring>
#include <string_view>

#include "fpag/base/numeric.h"

namespace base {

// Whether two names are the same bytes. A name is a handful of bytes and
// the comparison runs once per name a body resolves, so it reads the bytes
// in place: the call into the C library costs more than the comparison over
// the lengths names have.
[[nodiscard]] [[gnu::always_inline]] inline bool same_name(
    std::string_view lhs,
    std::string_view rhs) {
  const usize size = lhs.size();
  if (size != rhs.size()) {
    return false;
  }
  const char* left = lhs.data();
  const char* right = rhs.data();
  usize at = 0;
  while (size - at >= 8) {
    u64 words[2] = {0, 0};
    std::memcpy(&words[0], left + at, 8);
    std::memcpy(&words[1], right + at, 8);
    if (words[0] != words[1]) {
      return false;
    }
    at += 8;
  }
  if (size - at >= 4) {
    u32 words[2] = {0, 0};
    std::memcpy(&words[0], left + at, 4);
    std::memcpy(&words[1], right + at, 4);
    if (words[0] != words[1]) {
      return false;
    }
    at += 4;
  }
  if (size - at >= 2) {
    u16 words[2] = {0, 0};
    std::memcpy(&words[0], left + at, 2);
    std::memcpy(&words[1], right + at, 2);
    if (words[0] != words[1]) {
      return false;
    }
    at += 2;
  }
  return size == at || left[at] == right[at];
}

}  // namespace base
