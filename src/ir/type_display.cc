// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/type_display.h"

#include <string>

#include "base/nesting.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace ir {

namespace {

std::string type_display_at(const StorageState& state,
                            const SymbolTable& strings,
                            TypeIdx type,
                            u32 depth);

std::string argument_list(const StorageState& state,
                          const SymbolTable& strings,
                          TypeIdxRange args,
                          u32 depth) {
  std::string out;
  bool first = true;
  for (const TypeIdx arg : args) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += type_display_at(state, strings, arg, depth + 1);
  }
  return out;
}

std::string type_display_at(const StorageState& state,
                            const SymbolTable& strings,
                            TypeIdx type,
                            u32 depth) {
  if (depth > base::MAX_NESTING) {
    return "...";
  }
  const TypeNode& node = state.types[type];
  switch (node.tag) {
    case TypeTag::Void: return "()";
    case TypeTag::Never: return "!";
    case TypeTag::Error: return "<error>";
    case TypeTag::I1: return "bool";
    case TypeTag::I8: return "i8";
    case TypeTag::I16: return "i16";
    case TypeTag::I32: return "i32";
    case TypeTag::I64: return "i64";
    case TypeTag::U8: return "u8";
    case TypeTag::U16: return "u16";
    case TypeTag::U32: return "u32";
    case TypeTag::U64: return "u64";
    case TypeTag::F32: return "f32";
    case TypeTag::F64: return "f64";
    case TypeTag::Str: return "str";
    case TypeTag::Slice: {
      const SliceType& slice = state.slice_types[node.as_slice()];
      return "[" + type_display_at(state, strings, slice.element, depth + 1) +
             "]";
    }
    case TypeTag::Ref:
      return "&" + type_display_at(state, strings,
                                   state.ref_types[node.as_ref()].pointee,
                                   depth + 1);
    case TypeTag::MutRef:
      return "&mut " + type_display_at(state, strings,
                                       state.ref_types[node.as_ref()].pointee,
                                       depth + 1);
    case TypeTag::Ptr:
      return "*" + type_display_at(state, strings,
                                   state.ref_types[node.as_ref()].pointee,
                                   depth + 1);
    case TypeTag::RawPtr:
      return "*const " + type_display_at(state, strings,
                                         state.ref_types[node.as_ref()].pointee,
                                         depth + 1);
    case TypeTag::RawMutPtr:
      return "*mut " + type_display_at(state, strings,
                                       state.ref_types[node.as_ref()].pointee,
                                       depth + 1);
    case TypeTag::Array: {
      const ArrayType& array = state.array_types[node.as_array()];
      return "[" + type_display_at(state, strings, array.element, depth + 1) +
             "; " + std::to_string(array.count) + "]";
    }
    case TypeTag::Tuple: {
      const TupleType& tuple = state.tuple_types[node.as_tuple()];
      return "(" + argument_list(state, strings, tuple.elements, depth) + ")";
    }
    case TypeTag::Struct: {
      const StructType& structure = state.struct_types[node.as_struct()];
      std::string out(strings.get(structure.name));
      if (!structure.params.empty()) {
        out +=
            "<" + argument_list(state, strings, structure.params, depth) + ">";
      }
      return out;
    }
    case TypeTag::Enum: {
      const EnumType& enumeration = state.enum_types[node.as_enum()];
      std::string out(strings.get(enumeration.name));
      if (!enumeration.params.empty()) {
        out += "<" + argument_list(state, strings, enumeration.params, depth) +
               ">";
      }
      return out;
    }
    case TypeTag::Function: return "fn";
    case TypeTag::Func: {
      const FuncType& function = state.func_types[node.as_func()];
      return "fn(" + argument_list(state, strings, function.params, depth) +
             ") -> " + type_display_at(state, strings, function.ret, depth + 1);
    }
    case TypeTag::Const:
      return "const " +
             std::to_string(state.const_types[node.as_const()].value);
    case TypeTag::Param: return "T" + std::to_string(node.as_param().value);
  }
  return "?";
}

}  // namespace

std::string type_display(const StorageState& state,
                         const SymbolTable& strings,
                         TypeIdx type) {
  return type_display_at(state, strings, type, 0);
}

}  // namespace ir
