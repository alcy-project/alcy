// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "ir/opcode.h"
#include "ir/type.h"

namespace ir {

constexpr std::string_view format_as(const Opcode c) {
  return std::string_view{opcode_to_str(c)};
}

constexpr std::string_view format_as(const TypeTag t) {
  return std::string_view{type_to_str(t)};
}

}  // namespace ir
