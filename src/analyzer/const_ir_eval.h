// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "analyzer/comp_eval.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "comp/ir_interpreter.h"
#include "diag/span.h"
#include "ir/common.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

// The data an emitter needs to resolve checked tables. Every resolver is
// a member function over the explicit `EvalSite`; `site` changes when a
// comp callee in another module is inlined.
struct EmitContext {
  CompEval& evaluator;
  comp::CompScope& scope;
  EvalSite site;

  ir::TypeIdx type_of(ast::ExprIdx expr) const;
  ir::TypeIdx layout_type_of(ast::ExprIdx expr) const;
  bool path_value(ast::PathIdx path, comp::CompVal& out) const;
  bool comp_fn(ast::ExprIdx call, CompFnInfo& info) const;
  bool field_index(ir::TypeIdx type, std::string_view name, u32& index) const;
  bool variant_index(ir::TypeIdx type, std::string_view name, u32& index) const;

  EmitContext(CompEval& evaluator_in,
              comp::CompScope& scope_in,
              EvalSite site_in);
  EmitContext with_site(EvalSite next) const;
};

// Evaluates a checked constant expression through the IR interpreter,
// building a synthetic function in `builder`. Returns false without a
// value for any shape it does not carry and records the reason in
// `*error`. `type` is the checked type of the initializer.
bool evaluate_const_ir(const ast::AstArena& ast,
                       ast::ExprIdx expr,
                       ir::TypeIdx type,
                       ir::StorageBuilder& builder,
                       ir::PointerWidth width,
                       const EmitContext& ctx,
                       u64& quota,
                       u32& depth,
                       comp::CompVal& out,
                       comp::IrRunError* error = nullptr,
                       diag::Span* failure_span = nullptr);

}  // namespace analyzer
