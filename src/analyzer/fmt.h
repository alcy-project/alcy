// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "ir/type.h"

namespace analyzer {

// Shared format-string support for `fmt::write` checking (analyzer)
// and expansion (lowering, which reuses these over evaluated bytes).
// Decoding MUST match the runtime literal rules byte for byte.

struct FmtPiece {
  bool is_arg = false;
  std::string literal;
  u32 arg = 0;
};

enum class FmtError : u8 {
  None,
  UnclosedBrace,
  UnmatchedBrace,
  Specifier,
};

// Decodes string-literal escapes exactly like lowering.
inline std::string unescape_format_string(std::string_view spelling) {
  std::string bytes;
  if (spelling.size() >= 2) {
    spelling.remove_prefix(1);
    spelling.remove_suffix(1);
  }
  for (usize i = 0; i < spelling.size(); ++i) {
    const char c = spelling[i];
    if (c != '\\' || i + 1 >= spelling.size()) {
      bytes.push_back(c);
      continue;
    }
    const char esc = spelling[++i];
    switch (esc) {
      case 'n': bytes.push_back('\n'); break;
      case 't': bytes.push_back('\t'); break;
      case 'r': bytes.push_back('\r'); break;
      case '\\': bytes.push_back('\\'); break;
      case '"': bytes.push_back('"'); break;
      case '0': bytes.push_back('\0'); break;
      default: bytes.push_back(esc); break;
    }
  }
  return bytes;
}

struct FmtParse {
  std::vector<FmtPiece> pieces;
  u32 placeholders = 0;
  FmtError error = FmtError::None;
  usize error_pos = 0;
};

// Splits decoded bytes into literal pieces and sequential `{}`
// placeholders. `{{`/`}}` escape; anything else in braces errors.
inline FmtParse parse_format_string(const std::string& bytes) {
  FmtParse result;
  std::string literal;
  auto flush = [&]() {
    if (!literal.empty()) {
      result.pieces.push_back(FmtPiece{false, literal, 0});
      literal.clear();
    }
  };
  for (usize i = 0; i < bytes.size();) {
    if (bytes[i] == '{') {
      if (i + 1 >= bytes.size()) {
        result.error = FmtError::UnclosedBrace;
        result.error_pos = i;
        return result;
      }
      if (bytes[i + 1] == '{') {
        literal.push_back('{');
        i += 2;
        continue;
      }
      if (bytes[i + 1] != '}') {
        result.error = FmtError::Specifier;
        result.error_pos = i;
        return result;
      }
      flush();
      result.pieces.push_back(FmtPiece{true, {}, result.placeholders++});
      i += 2;
      continue;
    }
    if (bytes[i] == '}') {
      if (i + 1 < bytes.size() && bytes[i + 1] == '}') {
        literal.push_back('}');
        i += 2;
        continue;
      }
      result.error = FmtError::UnmatchedBrace;
      result.error_pos = i;
      return result;
    }
    literal.push_back(bytes[i]);
    ++i;
  }
  flush();
  return result;
}

// Closed convertible set: integers, bools, and strings.
inline bool is_formattable_tag(ir::TypeTag tag) {
  return tag == ir::TypeTag::I1 || tag == ir::TypeTag::Str ||
         (tag >= ir::TypeTag::I8 && tag <= ir::TypeTag::U64);
}

}  // namespace analyzer
