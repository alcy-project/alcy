// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ir/type.h"

namespace ir {

inline constexpr bool is_integer_type(TypeTag tag) {
  using T = TypeTag;
  switch (tag) {
    case T::I1:
    case T::I8:
    case T::I16:
    case T::I32:
    case T::I64:
    // case T::I128:
    case T::U8:
    case T::U16:
    case T::U32:
    case T::U64:
      // case T::U128:
      return true;
    default: return false;
  }
}

inline constexpr bool is_signed_integer_type(TypeTag tag) {
  using T = TypeTag;
  switch (tag) {
    // Currently not containing I1 as a signed integer
    // case T::I1:
    case T::I8:
    case T::I16:
    case T::I32:
    case T::I64: return true;
    default: return false;
  }
}

inline constexpr bool is_float_type(TypeTag tag) {
  using T = TypeTag;
  switch (tag) {
    case T::F32:
    case T::F64: return true;
    default: return false;
  }
}

// The unsigned tag of the same width, for the tags that have one.
// Reinterpreting a signed value through it preserves every bit, which
// is how a magnitude gets widened without re-extending the sign.
inline constexpr TypeTag unsigned_integer_type(TypeTag tag) {
  using T = TypeTag;
  switch (tag) {
    case T::I8: return T::U8;
    case T::I16: return T::U16;
    case T::I32: return T::U32;
    case T::I64: return T::U64;
    default: return tag;
  }
}

}  // namespace ir
