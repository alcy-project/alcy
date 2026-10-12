// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "ir/type.h"
#include "text/unescape.h"

namespace analyzer {

// Shared format-string support for `fmt::write` checking (analyzer)
// and expansion (lowering, which reuses these over evaluated bytes).
// Decoding MUST match the runtime literal rules byte for byte.

// A placeholder without a `{:.N}` precision.
inline constexpr u8 FMT_NO_PRECISION = 0xFF;
// The largest accepted precision. The expansion's scratch buffers are
// compile-time constants sized from it.
inline constexpr u8 FMT_MAX_PRECISION = 17;

struct FmtPiece {
  bool is_arg = false;
  std::string literal;
  u32 arg = 0;
  // Fractional digits for a float argument, from `{:.N}`; the default
  // when the placeholder names none.
  u8 precision = FMT_NO_PRECISION;
};

enum class FmtError : u8 {
  None,
  UnclosedBrace,
  UnmatchedBrace,
  Specifier,
};

// Decodes string-literal escapes exactly like lowering, which shares
// the implementation so the two cannot drift.
inline std::string unescape_format_string(std::string_view spelling) {
  return text::unescape_string(spelling);
}

struct FmtTemplate {
  std::vector<FmtPiece> pieces;
  u32 placeholders = 0;
  FmtError error = FmtError::None;
  usize error_pos = 0;
};

// Splits decoded bytes into literal pieces and sequential `{}`
// placeholders. `{{`/`}}` escape; anything else in braces errors.
inline FmtTemplate parse_format_string(const std::string& bytes) {
  FmtTemplate result;
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
        // `{:.Nf}`: the fixed-point fractional digits of a float
        // argument. The `f` names the conversion explicitly and is
        // optional: `{:.N}` is the same piece. A linter may later
        // require the explicit spelling.
        if (bytes[i + 1] != ':' || i + 2 >= bytes.size() ||
            bytes[i + 2] != '.') {
          result.error = FmtError::Specifier;
          result.error_pos = i;
          return result;
        }
        usize j = i + 3;
        u32 value = 0;
        u32 count = 0;
        while (j < bytes.size() && bytes[j] >= '0' && bytes[j] <= '9') {
          value = value * 10 + static_cast<u32>(bytes[j] - '0');
          ++count;
          ++j;
        }
        if (count > 0 && j < bytes.size() && bytes[j] == 'f') {
          ++j;
        }
        if (count == 0 || value > FMT_MAX_PRECISION || j >= bytes.size() ||
            bytes[j] != '}') {
          result.error = FmtError::Specifier;
          result.error_pos = i;
          return result;
        }
        flush();
        result.pieces.push_back(
            FmtPiece{true, {}, result.placeholders++, static_cast<u8>(value)});
        i = j + 1;
        continue;
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

// Closed convertible set: integers, bools, strings, and floats.
inline bool is_formattable_tag(ir::TypeTag tag) {
  return tag == ir::TypeTag::I1 || tag == ir::TypeTag::Str ||
         tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64 ||
         (tag >= ir::TypeTag::I8 && tag <= ir::TypeTag::U64);
}

// A `{:.N}` precision applies to floats only: no other convertible
// type has fractional digits to fill.
inline bool fmt_piece_matches_argument(const FmtPiece& piece, ir::TypeTag tag) {
  if (piece.precision == FMT_NO_PRECISION) {
    return true;
  }
  return tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64;
}

}  // namespace analyzer
