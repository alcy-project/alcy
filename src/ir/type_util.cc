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

constexpr u8 UNANSWERED = 0;
constexpr u8 NO_PARAM = 1;
constexpr u8 NAMES_PARAM = 2;
constexpr u8 ON_STACK = 3;

// Whether a type names a `Param` anywhere its parts can. A nominal
// instance's fields are walked too: an instance minted before a
// substitution finished still holds one there. Answers land in the
// state's memo, so a type is walked once per run; a type the walk
// reaches through itself is on the stack, and a cycle contributes
// nothing new.
bool type_names_param_at(const StorageState& state,
                         TypeIdx type,
                         std::vector<u8>& memo) {
  if (!type.is_valid() || type.idx >= state.types.size()) {
    return false;
  }
  if (type.idx >= memo.size()) {
    memo.resize(type.idx + 1, UNANSWERED);
  }
  const u8 held = memo[type.idx];
  if (held == NO_PARAM) {
    return false;
  }
  if (held == NAMES_PARAM) {
    return true;
  }
  if (held == ON_STACK) {
    return false;
  }
  memo[type.idx] = ON_STACK;
  const ir::TypeNode& node = state.types[type];
  using T = TypeTag;
  bool answer = false;
  switch (node.tag) {
    case T::Param: answer = true; break;
    case T::Ref:
    case T::MutRef:
    case T::RawPtr:
    case T::RawMutPtr:
      answer = type_names_param_at(
          state, state.ref_types[node.as_ref()].pointee, memo);
      break;
    case T::Array:
      answer = type_names_param_at(
          state, state.array_types[node.as_array()].element, memo);
      break;
    case T::Slice:
      answer = type_names_param_at(
          state, state.slice_types[node.as_slice()].element, memo);
      break;
    case T::Tuple:
      for (TypeIdx element : state.tuple_types[node.as_tuple()].elements) {
        if (type_names_param_at(state, element, memo)) {
          answer = true;
          break;
        }
      }
      break;
    case T::Func: {
      const FuncType& shape = state.func_types[node.as_func()];
      for (TypeIdx param : shape.params) {
        if (type_names_param_at(state, param, memo)) {
          answer = true;
          break;
        }
      }
      if (!answer) {
        answer = type_names_param_at(state, shape.ret, memo);
      }
      break;
    }
    case T::Struct: {
      const StructType& shape = state.struct_types[node.as_struct()];
      for (TypeIdx arg : shape.params) {
        if (type_names_param_at(state, arg, memo)) {
          answer = true;
          break;
        }
      }
      if (!answer) {
        // A field can name one where the instance's arguments do not: an
        // instance minted before a substitution completed.
        for (TypeIdx field : shape.fields) {
          if (type_names_param_at(state, field, memo)) {
            answer = true;
            break;
          }
        }
      }
      break;
    }
    case T::Enum: {
      const EnumType& shape = state.enum_types[node.as_enum()];
      for (TypeIdx arg : shape.params) {
        if (type_names_param_at(state, arg, memo)) {
          answer = true;
          break;
        }
      }
      if (!answer) {
        for (EnumVariantTypeIdx vidx : shape.variants) {
          bool found = false;
          for (TypeIdx field : state.enum_variant_types[vidx].fields) {
            if (type_names_param_at(state, field, memo)) {
              found = true;
              break;
            }
          }
          if (found) {
            answer = true;
            break;
          }
        }
      }
      break;
    }
    default: break;
  }
  memo[type.idx] = answer ? NAMES_PARAM : NO_PARAM;
  return answer;
}

}  // namespace

bool type_names_param(const StorageState& state, TypeIdx type) {
  std::vector<u8>& memo = state.names_param_memo;
  return type_names_param_at(state, type, memo);
}

}  // namespace ir
