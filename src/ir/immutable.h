// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "debug/dcheck.h"
#include "debug/fatal.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/type.h"
#include "ir/type_util.h"

namespace ir {

struct Immutable {
  Type type;
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

  inline u64 as_u64_integer() const {
    DCHECK_MSG(is_integer_type(type),
               "called as_u64_integer with not integer type");
    switch (type) {
      case Type::I1: return static_cast<u64>(data.i1_value);
      case Type::I8: return static_cast<u64>(data.i8_value);
      case Type::I16: return static_cast<u64>(data.i16_value);
      case Type::I32: return static_cast<u64>(data.i32_value);
      case Type::I64: return static_cast<u64>(data.i64_value);
      case Type::U8: return static_cast<u64>(data.u8_value);
      case Type::U16: return static_cast<u64>(data.u16_value);
      case Type::U32: return static_cast<u64>(data.u32_value);
      case Type::U64: return static_cast<u64>(data.u64_value);
      default: UNREACHABLE();
    }
  }

  inline f64 as_f64_fp() const {
    DCHECK_MSG(is_float_type(type),
               "called as_f64_fp with not floating point type");
    switch (type) {
      case Type::F32: return static_cast<f64>(data.f32_value);
      case Type::F64: return static_cast<f64>(data.f64_value);
      default: UNREACHABLE();
    }
  }
};

}  // namespace ir
