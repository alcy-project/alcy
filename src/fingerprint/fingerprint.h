// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "base/hash.h"
#include "fpag/base/numeric.h"

namespace fingerprint {

using Digest = base::Digest128;

// A caller-delimited byte stream that both the compiler's build cache
// and the editor's source signature hash with the project's one hash.
// The caller owns field separation; the builder only appends in the
// order given, so two readers of the same inputs reach the same digest.
class Builder {
 public:
  Builder& reserve(usize capacity) {
    bytes_.reserve(capacity);
    return *this;
  }

  Builder& bytes(std::string_view data) {
    bytes_.append(data);
    return *this;
  }

  Builder& u8(u8 value) {
    bytes_.push_back(static_cast<char>(value));
    return *this;
  }

  // A 128-bit content digest in the little-endian byte order the IR
  // writer uses, so a digest crosses the same wire here as there.
  Builder& digest(Digest value);

  Digest finish() const { return base::hash128(bytes_); }

 private:
  std::string bytes_;
};

}  // namespace fingerprint
