// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>

#include "ir/opcode.h"
#include "ir/type.h"

namespace ir {

constexpr std::string_view format_as(const Opcode c) {
  return std::string_view{opcode_to_str(c)};
}

constexpr std::string_view format_as(const Type t) {
  return std::string_view{type_to_str(t)};
}

}  // namespace ir
