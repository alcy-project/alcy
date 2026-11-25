// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fpag/base/numeric.h"

namespace codegen {

// Read-only bytes one object carries: the string literals its code
// names, each interned once under a stable symbol. Lookups take a view,
// so a repeated literal costs no allocation. Both direct backends own
// one of these and place the bytes in their own object.
class RoData {
 public:
  [[nodiscard]] u32 intern(std::string_view bytes);

  [[nodiscard]] const std::string& name(u32 index) const {
    return names_[index];
  }
  [[nodiscard]] const std::string& bytes(u32 index) const {
    return bytes_[index];
  }
  [[nodiscard]] const std::vector<std::string>& names() const { return names_; }
  [[nodiscard]] const std::vector<std::string>& bytes() const { return bytes_; }

 private:
  struct Hash {
    using is_transparent = void;
    usize operator()(std::string_view bytes) const {
      return std::hash<std::string_view>{}(bytes);
    }
  };
  struct Equal {
    using is_transparent = void;
    bool operator()(std::string_view left, std::string_view right) const {
      return left == right;
    }
  };
  std::unordered_map<std::string, u32, Hash, Equal> index_;
  std::vector<std::string> names_;
  std::vector<std::string> bytes_;
};

}  // namespace codegen
