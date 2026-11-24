// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

// Read-only bytes one object carries: the string literals its code
// names, each interned once under a stable symbol.
class RoData {
 public:
  [[nodiscard]] const std::string& intern(std::string_view bytes);

  [[nodiscard]] const std::vector<std::string>& names() const { return names_; }
  [[nodiscard]] const std::vector<std::string>& bytes() const { return bytes_; }

 private:
  std::unordered_map<std::string, u32> index_;
  std::vector<std::string> names_;
  std::vector<std::string> bytes_;
};

}  // namespace codegen::riscv64
