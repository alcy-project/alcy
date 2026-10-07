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

// Suffixes were validated by checking; strip the longest known suffix
// and parse what remains (wrapping arithmetic matches release overflow
// semantics; checked overflow is later work).
u64 parse_numeric_value(std::string_view spelling) {
  constexpr std::string_view SUFFIXES[] = {
      "isize", "usize", "i8",  "i16", "i32", "i64",
      "u8",    "u16",   "u32", "u64", "f32", "f64",
  };

  // Strip the suffix if present.
  for (std::string_view suffix : SUFFIXES) {
    const bool has_suffix =
        spelling.size() > suffix.size() && spelling.ends_with(suffix);
    if (has_suffix) {
      spelling.remove_suffix(suffix.size());
      break;
    }
  }

  // Strip a 0x, 0b, or 0o prefix.
  u32 base = 10;
  const bool has_prefix = spelling.size() > 2 && spelling[0] == '0';
  if (has_prefix) {
    const char prefix_indicator = spelling[1];
    if (prefix_indicator == 'x' || prefix_indicator == 'X') {
      base = 16;
      spelling.remove_prefix(2);
    } else if (prefix_indicator == 'b' || prefix_indicator == 'B') {
      base = 2;
      spelling.remove_prefix(2);
    } else if (prefix_indicator == 'o' || prefix_indicator == 'O') {
      base = 8;
      spelling.remove_prefix(2);
    }
  }

  u64 value = 0;
  for (const char ch : spelling) {
    if (ch == '_') {
      continue;
    }

    const bool is_digit = (ch >= '0' && ch <= '9');
    const bool is_lower_hex = (ch >= 'a' && ch <= 'f');
    const bool is_upper_hex = (ch >= 'A' && ch <= 'F');
    u32 digit = 0;
    if (is_digit) {
      digit = static_cast<u32>(ch - '0');
    } else if (is_lower_hex) {
      digit = static_cast<u32>(ch - 'a' + 10);
    } else if (is_upper_hex) {
      digit = static_cast<u32>(ch - 'A' + 10);
    } else {
      continue;
    }
    value = value * base + digit;
  }
  return value;
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
