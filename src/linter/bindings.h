// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "linter/context.h"

namespace linter {

// One name a function or closure declares, and what the body does with
// it. Reads count every mention of the name anywhere in its scope;
// mutations count reassignments, mutable borrows, and method-call
// receivers, which is deliberately conservative: a method may take its
// receiver newly exclusively, and counting every receiver as a mutation
// keeps the unused-mut rule from flagging a binding that is only ever
// written through a method.
struct BindingUse {
  std::string_view name;
  // The identifier as written, which is where a finding points.
  diag::Span span;
  // Whether the declaration spelled mut.
  bool is_mut = false;
  // Whether the binding is a parameter rather than a let: the two are
  // reported with different wording.
  bool is_param = false;
  u32 reads = 0;
  u32 mutations = 0;
  // The bound name never used: a binding the desugarer freshened because
  // it shadows one in an enclosing scope. The unused and mut rules do not
  // need it; the shadow rule reads it.
  bool is_shadowed = false;
};

// Called once per binding, after the scope that declares it closes, so
// the counters are final.
using BindingCallback = void (*)(void* self, LintContext&, const BindingUse&);

// Walks one item's body and reports every binding it declares. An item
// that declares no body (a struct, a use) reports nothing. Closures
// nested in a function report their own bindings, so one call covers the
// whole subtree.
void analyze_bindings(LintContext& ctx,
                      ast::ItemIdx item,
                      void* self,
                      BindingCallback visit);

}  // namespace linter
