// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "base/hash.h"
#include "fpag/base/numeric.h"

namespace fingerprint {

using Digest = base::Digest128;

// A canonical, length-delimited byte stream that both the compiler's
// build cache and the editor's source signature hash with the project's
// one hash. The bytes are accumulated in the order the caller adds them,
// so two readers of the same inputs reach the same digest.
class Builder {
 public:
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

  // Length-prefixed bytes, for fields whose content can contain the
  // delimiters a neighbour uses.
  Builder& sized(std::string_view data);

  Digest finish() const { return base::hash128(bytes_); }

 private:
  std::string bytes_;
};

}  // namespace fingerprint
