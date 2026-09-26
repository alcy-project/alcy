// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "debug/dcheck.h"
#include "debug/fatal.h"
#include "fpag/base/numeric.h"
#include "fpag/base/union.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"

namespace ir {

enum class TypeTag : u8 {
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
  Array,
  Function,
  Enum,
  Never,
  Tuple,
  // Error recovery marker (never written by user code): failed resolutions
  // produce this so checking can continue without cascades. Must never
  // reach codegen; the cli aborts on any recorded error first.
  Error,
};

// Target pointer width: selects the mapping of isize/usize. Passed
// explicitly (never sniffed from the host) so cross builds stay honest.
enum class PointerWidth : u8 {
  W32,
  W64,
};

// Number of primitive tags below Struct. StorageBuilder pre-interns them in
// TypeTag order so that TypeIdx(static_cast<u32>(tag)) resolves without a
// table lookup. The Function tag is pre-interned right after them; Struct
// and Array nodes are only created via their factories.
constexpr u32 PRIMITIVE_TYPE_COUNT = static_cast<u32>(TypeTag::Struct);

// Resolves a pre-interned primitive tag to its table index without a lookup.
// StorageBuilder pre-interns these in TypeTag order (plus Function right
// after), so this mapping must stay in sync with intern_primitives().
constexpr TypeIdx primitive_idx(TypeTag tag) {
  if (tag == TypeTag::Function) {
    return TypeIdx(PRIMITIVE_TYPE_COUNT);
  }
  return TypeIdx(static_cast<u32>(tag));
}

struct StructType {
  str::StringPoolId name;
  // Field types in declaration order.
  TypeIdxRange fields;
  // Generic arguments this instantiation was built with, empty for a
  // non-generic declaration. A symbol names the arguments, so a type
  // has to carry its own.
  TypeIdxRange params;
};

struct ArrayType {
  TypeIdx element;
  u64 count;
};

struct EnumVariantType {
  str::StringPoolId name;
  // Payload field types in declaration order; empty for unit variants.
  TypeIdxRange fields;
};

struct EnumType {
  str::StringPoolId name;
  // Variants in declaration order; the index doubles as the discriminant.
  EnumVariantTypeIdxRange variants;
  // Generic arguments, as for StructType.
  TypeIdxRange params;
};

// Pointee of a Ref or MutRef node. Reference nodes are structurally
// interned by this payload (see StorageBuilder::reference_type); the
// pre-interned placeholder nodes predate payloads and never match.
struct RefType {
  TypeIdx pointee;
};

struct TupleType {
  // Element types in order.
  TypeIdxRange elements;
};

struct TypeNode {
  TypeTag tag;
  // Meaningful only for Struct/Array/Enum/Ref/MutRef/Tuple tags.
  base::
      Union<StructTypeIdx, ArrayTypeIdx, EnumTypeIdx, RefTypeIdx, TupleTypeIdx>
          data;

  inline StructTypeIdx as_struct() const {
    DCHECK_MSG(tag == TypeTag::Struct, "type node is not a struct");
    return data.get<StructTypeIdx>();
  }

  inline ArrayTypeIdx as_array() const {
    DCHECK_MSG(tag == TypeTag::Array, "type node is not an array");
    return data.get<ArrayTypeIdx>();
  }

  inline EnumTypeIdx as_enum() const {
    DCHECK_MSG(tag == TypeTag::Enum, "type node is not an enum");
    return data.get<EnumTypeIdx>();
  }

  inline RefTypeIdx as_ref() const {
    DCHECK_MSG(tag == TypeTag::Ref || tag == TypeTag::MutRef,
               "type node is not a reference");
    return data.get<RefTypeIdx>();
  }

  inline TupleTypeIdx as_tuple() const {
    DCHECK_MSG(tag == TypeTag::Tuple, "type node is not a tuple");
    return data.get<TupleTypeIdx>();
  }
};

constexpr const char* type_to_str(TypeTag tag) {
  using T = TypeTag;
  switch (tag) {
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
    case T::Array: return "array";
    case T::Function: return "function";
    case T::Enum: return "enum";
    case T::Never: return "never";
    case T::Tuple: return "tuple";
    case T::Error: return "error";
    default: UNREACHABLE();
  }
}

}  // namespace ir
