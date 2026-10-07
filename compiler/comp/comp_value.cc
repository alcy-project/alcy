// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "comp/comp_value.h"

#include <ranges>
#include <string>
#include <string_view>

#include "ir/type.h"
#include "text/unescape.h"

namespace comp {

bool is_signed(ir::TypeTag tag) {
  return tag == ir::TypeTag::I8 || tag == ir::TypeTag::I16 ||
         tag == ir::TypeTag::I32 || tag == ir::TypeTag::I64;
}

u32 int_bytes(ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I8:
    case ir::TypeTag::U8: return 1;
    case ir::TypeTag::I16:
    case ir::TypeTag::U16: return 2;
    case ir::TypeTag::I32:
    case ir::TypeTag::U32: return 4;
    default: return 8;
  }
}

u64 mask(ir::TypeTag tag) {
  const u32 bytes = int_bytes(tag);
  return bytes >= 8 ? ~static_cast<u64>(0)
                    : ((static_cast<u64>(1) << (bytes * 8)) - 1);
}

i64 sign_extend(u64 bits, ir::TypeTag tag) {
  const u32 bytes = int_bytes(tag);
  if (bytes >= 8) {
    return static_cast<i64>(bits);
  }
  const u64 sign_mask = mask(tag);
  const u64 sign = static_cast<u64>(1) << (bytes * 8 - 1);
  bits &= sign_mask;
  if ((bits & sign) != 0) {
    bits |= ~sign_mask;
  }
  return static_cast<i64>(bits);
}

bool truth(const CompVal& value) {
  if (value.value.tag == CompValue::Tag::Bool) {
    return value.value.bool_value;
  }
  return value.value.int_value != 0;
}

const CompVal* lookup(const CompScope& scope, std::string_view name) {
  for (usize i = scope.frames.size(); i-- > 0;) {
    for (const auto& binding : scope.frames[i] | std::views::reverse) {
      if (binding.first == name) {
        return &binding.second;
      }
    }
  }
  if (scope.outer != nullptr) {
    for (const auto& binding : *scope.outer | std::views::reverse) {
      if (binding.first == name) {
        return &binding.second;
      }
    }
  }
  return nullptr;
}

std::string unescape(std::string_view spelling) {
  return text::unescape_string(spelling);
}

}  // namespace comp
