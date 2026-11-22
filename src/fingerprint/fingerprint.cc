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

}  // namespace fingerprint
