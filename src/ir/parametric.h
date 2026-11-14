// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"

namespace ir {

// What a parametric call waits on.
enum class ParametricCalleeKind : u8 {
  // A generic item: the specialization substitutes `args` and clones
  // the function's body (ADR-0079).
  Generic,
  // A method a parameter's bound names: the specialization resolves
  // the spec's implementation for the concrete type (ADR-0080).
  Dispatch,
};

// A callee the parametric form cannot name: the definition-site check
// resolved the type arguments but not the function. The operand that
// carries this row is a call's callee, and the call's signature is that
// operand's type. No backend ever meets one; the specialization pass
// rewrites the call to the function the arguments decide.
struct ParametricCallee {
  ParametricCalleeKind kind = ParametricCalleeKind::Generic;
  // A Generic row's item, as the parametric function it lowered to.
  FunctionIdx function = FunctionIdx::invalid();
  // A Dispatch row's spec and method, by the names the source wrote:
  // a spec is a declaration, and its name is how the implementation is
  // found once the argument's type is known (ADR-0080).
  str::StringPoolId spec = str::INVALID_STRING_POOL_ID;
  str::StringPoolId method = str::INVALID_STRING_POOL_ID;
  // The type arguments the definition-site check resolved: the generic
  // item's arguments, or the parameter a dispatch acts on. The row
  // holds the range; the builder keeps the entries consecutive.
  TypeIdxRange args;
};

}  // namespace ir
