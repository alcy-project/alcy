// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "debug/fatal.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"

namespace ir {

enum class Type : u8 {
  Void,  // For function return type
  I1,    // bool
  I8,
  I16,
  I32,
  I64,
  // TODO: add these types
  // I128,
  U8,
  U16,
  U32,
  U64,
  // U128,
  // F16,
  F32,
  F64,
  // F128,
  Str,
  Ptr,     // Opaque pointer
  Ref,     // Immutable reference (region tracked)
  MutRef,  // Mutable reference  (exclusive, region tracked)

  Struct,
  Function,
};

struct StructTypeMetaData {
  TypeIdx fields_head;
  u32 field_count;
};

struct ArrayTypeMetaData {
  Type element_type;
  u64 element_count;
};

constexpr const char* type_to_str(Type type) {
  using T = Type;
  switch (type) {
    case T::Void: return "void";
    case T::I1: return "i1";
    case T::I8: return "i8";
    case T::I16: return "i16";
    case T::I32: return "i32";
    case T::I64: return "i64";
    // case T::I128: return "i128";
    case T::U8: return "u8";
    case T::U16: return "u16";
    case T::U32: return "u32";
    case T::U64: return "u64";
    // case T::U128: return "u128";
    // case T::F16: return "f16";
    case T::F32: return "f32";
    case T::F64: return "f64";
    // case T::F128: return "f128";
    case T::Str: return "str";
    case T::Ptr: return "ptr";
    case T::Ref: return "ref";
    case T::MutRef: return "mut_ref";

    case T::Struct: return "struct";
    case T::Function: return "function";
    default: UNREACHABLE();
  }
}

}  // namespace ir
