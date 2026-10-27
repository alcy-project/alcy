// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/typing.h"

#include <string>

#include "base/nesting.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace lsp {

namespace {

std::string type_name_at(const ir::StorageState& state,
                         const ir::SymbolTable& strings,
                         ir::TypeIdx type,
                         u32 depth);

std::string argument_list(const ir::StorageState& state,
                          const ir::SymbolTable& strings,
                          ir::TypeIdxRange args,
                          u32 depth) {
  std::string out;
  bool first = true;
  for (const ir::TypeIdx arg : args) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += type_name_at(state, strings, arg, depth + 1);
  }
  return out;
}

std::string type_name_at(const ir::StorageState& state,
                         const ir::SymbolTable& strings,
                         ir::TypeIdx type,
                         u32 depth) {
  if (depth > base::MAX_NESTING) {
    return "...";
  }
  const ir::TypeNode& node = state.types[type];
  switch (node.tag) {
    case ir::TypeTag::Void: return "()";
    case ir::TypeTag::Never: return "!";
    case ir::TypeTag::Error: return "<error>";
    case ir::TypeTag::I1: return "bool";
    case ir::TypeTag::I8: return "i8";
    case ir::TypeTag::I16: return "i16";
    case ir::TypeTag::I32: return "i32";
    case ir::TypeTag::I64: return "i64";
    case ir::TypeTag::U8: return "u8";
    case ir::TypeTag::U16: return "u16";
    case ir::TypeTag::U32: return "u32";
    case ir::TypeTag::U64: return "u64";
    case ir::TypeTag::F32: return "f32";
    case ir::TypeTag::F64: return "f64";
    case ir::TypeTag::Str: return "str";
    case ir::TypeTag::Slice: {
      const ir::SliceType& slice = state.slice_types[node.as_slice()];
      return "[" + type_name_at(state, strings, slice.element, depth + 1) + "]";
    }
    case ir::TypeTag::Ref:
      return "&" + type_name_at(state, strings,
                                state.ref_types[node.as_ref()].pointee,
                                depth + 1);
    case ir::TypeTag::MutRef:
      return "&mut " + type_name_at(state, strings,
                                    state.ref_types[node.as_ref()].pointee,
                                    depth + 1);
    case ir::TypeTag::Ptr:
      return "*" + type_name_at(state, strings,
                                state.ref_types[node.as_ref()].pointee,
                                depth + 1);
    case ir::TypeTag::RawPtr:
      return "*const " + type_name_at(state, strings,
                                      state.ref_types[node.as_ref()].pointee,
                                      depth + 1);
    case ir::TypeTag::RawMutPtr:
      return "*mut " + type_name_at(state, strings,
                                    state.ref_types[node.as_ref()].pointee,
                                    depth + 1);
    case ir::TypeTag::Array: {
      const ir::ArrayType& array = state.array_types[node.as_array()];
      return "[" + type_name_at(state, strings, array.element, depth + 1) +
             "; " + std::to_string(array.count) + "]";
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType& tuple = state.tuple_types[node.as_tuple()];
      return "(" + argument_list(state, strings, tuple.elements, depth) + ")";
    }
    case ir::TypeTag::Struct: {
      const ir::StructType& structure = state.struct_types[node.as_struct()];
      std::string out(strings.get(structure.name));
      if (!structure.params.empty()) {
        out +=
            "<" + argument_list(state, strings, structure.params, depth) + ">";
      }
      return out;
    }
    case ir::TypeTag::Enum: {
      const ir::EnumType& enumeration = state.enum_types[node.as_enum()];
      std::string out(strings.get(enumeration.name));
      if (!enumeration.params.empty()) {
        out += "<" + argument_list(state, strings, enumeration.params, depth) +
               ">";
      }
      return out;
    }
    case ir::TypeTag::Function: return "fn";
    case ir::TypeTag::Func: {
      const ir::FuncType& function = state.func_types[node.as_func()];
      return "fn(" + argument_list(state, strings, function.params, depth) +
             ") -> " + type_name_at(state, strings, function.ret, depth + 1);
    }
    case ir::TypeTag::Const:
      return "const " +
             std::to_string(state.const_types[node.as_const()].value);
    case ir::TypeTag::Param: return "T" + std::to_string(node.as_param().value);
  }
  return "?";
}

}  // namespace

std::string type_name(const ir::StorageState& state,
                      const ir::SymbolTable& strings,
                      ir::TypeIdx type) {
  return type_name_at(state, strings, type, 0);
}

}  // namespace lsp
