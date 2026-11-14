// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/type_util.h"

#include <vector>

#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace ir {

namespace {

// Whether a type names a `Param` anywhere its parts can. A nominal
// instance's fields are walked too: an instance minted before a
// substitution finished still holds one there.
bool type_names_param_at(const StorageState& state,
                         TypeIdx type,
                         std::vector<u32>& path) {
  if (!type.is_valid() || type.idx >= state.types.size()) {
    return false;
  }
  // A recursive type reaches itself; a cycle contributes nothing new.
  for (const u32 seen : path) {
    if (seen == type.idx) {
      return false;
    }
  }
  path.push_back(type.idx);
  const ir::TypeNode& node = state.types[type];
  using T = TypeTag;
  switch (node.tag) {
    case T::Param: return true;
    case T::Ref:
    case T::MutRef:
    case T::RawPtr:
    case T::RawMutPtr:
      return type_names_param_at(state, state.ref_types[node.as_ref()].pointee,
                                 path);
    case T::Array:
      return type_names_param_at(
          state, state.array_types[node.as_array()].element, path);
    case T::Slice:
      return type_names_param_at(
          state, state.slice_types[node.as_slice()].element, path);
    case T::Tuple: {
      for (TypeIdx element : state.tuple_types[node.as_tuple()].elements) {
        if (type_names_param_at(state, element, path)) {
          return true;
        }
      }
      return false;
    }
    case T::Func: {
      const FuncType& shape = state.func_types[node.as_func()];
      for (TypeIdx param : shape.params) {
        if (type_names_param_at(state, param, path)) {
          return true;
        }
      }
      return type_names_param_at(state, shape.ret, path);
    }
    case T::Struct: {
      const StructType& shape = state.struct_types[node.as_struct()];
      for (TypeIdx arg : shape.params) {
        if (type_names_param_at(state, arg, path)) {
          return true;
        }
      }
      // A field can name one where the instance's arguments do not: an
      // instance minted before a substitution completed.
      for (TypeIdx field : shape.fields) {
        if (type_names_param_at(state, field, path)) {
          return true;
        }
      }
      return false;
    }
    case T::Enum: {
      const EnumType& shape = state.enum_types[node.as_enum()];
      for (TypeIdx arg : shape.params) {
        if (type_names_param_at(state, arg, path)) {
          return true;
        }
      }
      for (EnumVariantTypeIdx vidx : shape.variants) {
        for (TypeIdx field : state.enum_variant_types[vidx].fields) {
          if (type_names_param_at(state, field, path)) {
            return true;
          }
        }
      }
      return false;
    }
    default: return false;
  }
}

}  // namespace

bool type_names_param(const StorageState& state,
                      TypeIdx type,
                      std::vector<u32>& path) {
  path.clear();
  return type_names_param_at(state, type, path);
}

bool type_names_param(const StorageState& state, TypeIdx type) {
  std::vector<u32> path;
  return type_names_param(state, type, path);
}

}  // namespace ir
