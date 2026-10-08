// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/type.h"

namespace comp {

// A compile-time value. Integers ride as u64 with their type attached
// and wrap like the opcodes that would produce them, two's-complement
// for negation; aggregates carry positional fields.
struct CompValue {
  enum class Tag : u8 {
    Void,
    Int,
    Bool,
    Str,
    Tuple,
    Array,
    Struct,
    Enum,
  };
  Tag tag = Tag::Void;
  u64 int_value = 0;
  bool bool_value = false;
  std::string str_value;
  std::vector<CompValue> fields;
  u32 variant = 0;
};

struct CompVal {
  CompValue value;
  ir::TypeIdx type = ir::TypeIdx(base::INVALID_IDX);
};

// What an evaluated block did: produced a value, or left through
// break, continue, or return.
struct CompFlow {
  enum class Kind : u8 { Value, Break, Continue, Return };
  Kind kind = Kind::Value;
  CompVal value;
};

// Lexical comp bindings: persistent per-function bindings plus
// evaluation-local frames.
struct CompScope {
  std::vector<std::pair<std::string_view, CompVal>>* outer = nullptr;
  std::vector<std::vector<std::pair<std::string_view, CompVal>>> frames;
};

// The integer semantics every reader of a comp value shares.
bool is_signed(ir::TypeTag tag);
u32 int_bytes(ir::TypeTag tag);
u64 mask(ir::TypeTag tag);
i64 sign_extend(u64 bits, ir::TypeTag tag);
// A literal's magnitude: strip the longest known suffix and the 0x,
// 0b, or 0o prefix, then parse the digits (wrapping arithmetic).
u64 parse_numeric_value(std::string_view spelling);
// The truth a comp value carries; an integer is true when nonzero.
bool truth(const CompVal& value);
// The binding `name` in `scope`, innermost frame first, or nullptr.
const CompVal* lookup(const CompScope& scope, std::string_view name);
// Unescapes with exactly the runtime literal rules, so a comp string
// and the lowered literal agree byte for byte.
std::string unescape(std::string_view spelling);

}  // namespace comp
