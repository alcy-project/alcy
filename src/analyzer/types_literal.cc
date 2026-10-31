// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/checker.h"
#include "analyzer/diag_code.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

bool Checker::classify_suffix(std::string_view spelling,
                              ir::TypeTag& tag,
                              bool& is_float,
                              diag::Span span) {
  using TT = ir::TypeTag;
  // Split the spelling into its digit body and whatever follows. Both
  // the base prefix and the digit set belong to the number, so the
  // suffix starts right after them: `0xFF` has none (its digits are
  // letters), `0xFFi32` is `i32`, and `42i128` is a suffix this
  // compiler has no type for. Scanning for a trailing letter run
  // instead would miss every suffix that carries a digit.
  usize body = 0;
  u32 base = 10;
  if (spelling.size() > 2 && spelling[0] == '0') {
    switch (spelling[1]) {
      case 'x':
      case 'X':
        body = 2;
        base = 16;
        break;
      case 'b':
      case 'B':
        body = 2;
        base = 2;
        break;
      case 'o':
      case 'O':
        body = 2;
        base = 8;
        break;
      default: break;
    }
  }
  const auto in_base = [base](char c) {
    if (c >= '0' && c <= '9') {
      return static_cast<u32>(c - '0') < base;
    }
    if (base == 16) {
      return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    return false;
  };
  usize digit_end = body;
  while (digit_end < spelling.size()) {
    const char c = spelling[digit_end];
    if (c == '_' || in_base(c)) {
      ++digit_end;
      continue;
    }
    // A decimal literal may carry a fraction point and an exponent,
    // and both belong to the number rather than to the suffix. The
    // exponent only counts when digits actually follow its optional
    // sign, so `1e` leaves the `e` to be reported.
    if (base != 10) {
      break;
    }
    if (c == '.') {
      ++digit_end;
      continue;
    }
    if (c == 'e' || c == 'E') {
      usize next = digit_end + 1;
      if (next < spelling.size() &&
          (spelling[next] == '+' || spelling[next] == '-')) {
        ++next;
      }
      if (next >= spelling.size() || spelling[next] < '0' ||
          spelling[next] > '9') {
        break;
      }
      digit_end = next;
      continue;
    }
    break;
  }
  const std::string_view suffix = spelling.substr(digit_end);
  if (suffix.empty()) {
    return false;
  }
  struct Suffix {
    std::string_view text;
    ir::TypeTag tag;
    bool is_float;
  };
  constexpr Suffix SUFFIXES[] = {
      {"isize", TT::I64, false}, {"usize", TT::U64, false},
      {"i8", TT::I8, false},     {"i16", TT::I16, false},
      {"i32", TT::I32, false},   {"i64", TT::I64, false},
      {"u8", TT::U8, false},     {"u16", TT::U16, false},
      {"u32", TT::U32, false},   {"u64", TT::U64, false},
      {"f32", TT::F32, true},    {"f64", TT::F64, true},
  };
  for (const Suffix& known : SUFFIXES) {
    if (suffix != known.text) {
      continue;
    }
    tag = known.tag;
    if (known.text == "isize") {
      tag = width == ir::PointerWidth::W64 ? TT::I64 : TT::I32;
    } else if (known.text == "usize") {
      tag = width == ir::PointerWidth::W64 ? TT::U64 : TT::U32;
    }
    is_float = known.is_float;
    return true;
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerUnsupportedLiteralSuffix>(
      DiagCode::UnsupportedType, span, suffix);
  (void)index;
  tag = TT::Error;
  return true;
}

// Whether an integer literal's magnitude fits the type it was given.
// A signed minimum is one past the positive maximum, so a negation has
// room the positive form does not.

bool Checker::int_literal_fits(std::string_view spelling,
                               ir::TypeTag tag,
                               bool negative) {
  bool overflow = false;
  const u64 magnitude = comp::parse_numeric_value(spelling, &overflow);
  // Past u64 is past every type, and the parse wraps there.
  if (overflow) {
    return false;
  }
  switch (tag) {
    case ir::TypeTag::U8: return magnitude <= (negative ? 0ull : 0xFFull);
    case ir::TypeTag::U16: return magnitude <= (negative ? 0ull : 0xFFFFull);
    case ir::TypeTag::U32:
      return magnitude <= (negative ? 0ull : 0xFFFFFFFFull);
    case ir::TypeTag::U64: return !negative || magnitude == 0;
    case ir::TypeTag::I8: return magnitude <= (negative ? 0x80ull : 0x7Full);
    case ir::TypeTag::I16:
      return magnitude <= (negative ? 0x8000ull : 0x7FFFull);
    case ir::TypeTag::I32:
      return magnitude <= (negative ? 0x80000000ull : 0x7FFFFFFFull);
    case ir::TypeTag::I64:
      return magnitude <=
             (negative ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull);
    default: return true;
  }
}

ir::TypeIdx Checker::check_literal(ast::LiteralIdx value,
                                   const ir::TypeIdx* expected) {
  using LK = ast::LiteralKind;
  const ast::Literal& lit = ast.literals[value];
  switch (lit.kind) {
    case LK::Bool: {
      const ir::TypeIdx type = builder.primitive(ir::TypeTag::I1);
      if (expected != nullptr) {
        return unify(*expected, type, lit.span, "boolean literal");
      }
      return type;
    }
    case LK::String: {
      const ir::TypeIdx type = builder.primitive(ir::TypeTag::Str);
      if (expected != nullptr) {
        return unify(*expected, type, lit.span, "string literal");
      }
      return type;
    }
    case LK::Char: {
      const u32 index = bag.emit<i18n::Key::AnalyzerCharacterLiteralDeferred>(
          DiagCode::UnsupportedType, lit.span);
      (void)index;
      return error_type();
    }
    case LK::Integer: {
      ir::TypeTag tag = ir::TypeTag::I32;
      bool is_float = false;
      const bool has_suffix =
          classify_suffix(lit.spelling, tag, is_float, lit.span);
      if (tag == ir::TypeTag::Error) {
        return error_type();
      }
      if (is_float) {
        const u32 index = bag.emit<i18n::Key::AnalyzerFloatSuffixOnInteger>(
            DiagCode::TypeMismatch, lit.span);
        (void)index;
        return error_type();
      }
      const bool negative = lit.is_negative != literal_negated_;
      if (!has_suffix && expected != nullptr &&
          tag_of(*expected) == ir::TypeTag::Param) {
        // The parameter stands for an integer type the instantiation
        // chooses: the definition-site check adopts it, and the width
        // settles where the argument is known (ADR-0079).
        return *expected;
      }
      if (!has_suffix && expected != nullptr &&
          is_integer_tag(tag_of(*expected))) {
        const ir::TypeTag wanted = tag_of(*expected);
        if (!int_literal_fits(lit.spelling, wanted, negative)) {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerIntegerLiteralOutOfRange>(
                  DiagCode::TypeMismatch, lit.span);
          (void)index;
          return error_type();
        }
        return *expected;
      }
      // The literal takes its suffix's type, or the default. The
      // magnitude must fit either: without the check a value past the
      // type truncated silently to a different number.
      if (!int_literal_fits(lit.spelling, tag, negative)) {
        const u32 index = bag.emit<i18n::Key::AnalyzerIntegerLiteralOutOfRange>(
            DiagCode::TypeMismatch, lit.span);
        (void)index;
        return error_type();
      }
      const ir::TypeIdx type = builder.primitive(tag);
      if (expected != nullptr) {
        return unify(*expected, type, lit.span, "integer literal");
      }
      return type;
    }
    case LK::Float: {
      ir::TypeTag tag = ir::TypeTag::F64;
      bool is_float = true;
      const bool has_suffix =
          classify_suffix(lit.spelling, tag, is_float, lit.span);
      if (tag == ir::TypeTag::Error) {
        return error_type();
      }
      if (has_suffix && !is_float_tag(tag)) {
        const u32 index = bag.emit<i18n::Key::AnalyzerIntegerSuffixOnFloat>(
            DiagCode::TypeMismatch, lit.span);
        (void)index;
        return error_type();
      }
      if (!has_suffix && expected != nullptr &&
          (is_float_tag(tag_of(*expected)) ||
           tag_of(*expected) == ir::TypeTag::Param)) {
        return *expected;
      }
      const ir::TypeIdx type = builder.primitive(tag);
      if (expected != nullptr) {
        return unify(*expected, type, lit.span, "float literal");
      }
      return type;
    }
  }
}

}  // namespace analyzer
