// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fpag/base/numeric.h"

namespace codegen::x86_64 {

// The read-only data one object carries: the bytes a program's string
// literals and the runtime's own text name, each under a symbol the
// relocation that reaches it spells. Two bodies that name the same bytes
// name one entry, so the section holds each once.
class RoData {
 public:
  // The symbol name for one byte string, making the entry on first use.
  [[nodiscard]] const std::string& intern(std::string_view bytes);

  // The interned strings in definition order, paired with the symbol
  // each one answers to. The order is what makes an object
  // reproducible.
  [[nodiscard]] const std::vector<std::string>& names() const { return names_; }
  [[nodiscard]] const std::vector<std::string>& bytes() const { return bytes_; }

 private:
  std::unordered_map<std::string, u32> index_;
  std::vector<std::string> names_;
  std::vector<std::string> bytes_;
};

}  // namespace codegen::x86_64
