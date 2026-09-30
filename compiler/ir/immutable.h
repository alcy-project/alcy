// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "debug/dcheck.h"
#include "debug/fatal.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/type.h"
#include "ir/type_util.h"

namespace ir {

struct Immutable {
  TypeIdx type;
  union {
    bool i1_value;
    i8 i8_value;
    i16 i16_value;
    i32 i32_value;
    i64 i64_value;
    // i128 i128_value;
    u8 u8_value;
    u16 u16_value;
    u32 u32_value;
    u64 u64_value;
    // u128 u128_value;
    f32 f32_value;
    f64 f64_value;
    str::StringPoolId str_id_value;
    u64 ptr;
    u64 mutptr;
    u64 ref;
    u64 mutref;
  } data;

  constexpr u64 as_u64_integer(TypeTag tag) const {
    DCHECK_MSG(is_integer_type(tag),
               "called as_u64_integer with not integer type");
    switch (tag) {
      case TypeTag::I1: return static_cast<u64>(data.i1_value);
      case TypeTag::I8: return static_cast<u64>(data.i8_value);
      case TypeTag::I16: return static_cast<u64>(data.i16_value);
      case TypeTag::I32: return static_cast<u64>(data.i32_value);
      case TypeTag::I64: return static_cast<u64>(data.i64_value);
      case TypeTag::U8: return static_cast<u64>(data.u8_value);
      case TypeTag::U16: return static_cast<u64>(data.u16_value);
      case TypeTag::U32: return static_cast<u64>(data.u32_value);
      case TypeTag::U64: return static_cast<u64>(data.u64_value);
      default: UNREACHABLE();
    }
  }

  constexpr f64 as_f64_fp(TypeTag tag) const {
    DCHECK_MSG(is_float_type(tag),
               "called as_f64_fp with not floating point type");
    switch (tag) {
      case TypeTag::F32: return static_cast<f64>(data.f32_value);
      case TypeTag::F64: return static_cast<f64>(data.f64_value);
      default: UNREACHABLE();
    }
  }
};

}  // namespace ir
