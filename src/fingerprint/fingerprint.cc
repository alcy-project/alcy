// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fingerprint/fingerprint.h"

#include <string_view>

namespace fingerprint {

Builder& Builder::digest(Digest value) {
  const auto bytes = value.bytes();
  bytes_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return *this;
}

Builder& Builder::sized(std::string_view data) {
  const u32 length = static_cast<u32>(data.size());
  for (u32 shift = 0; shift < 32; shift += 8) {
    bytes_.push_back(static_cast<char>((length >> shift) & 0xFFu));
  }
  bytes_.append(data);
  return *this;
}

}  // namespace fingerprint
