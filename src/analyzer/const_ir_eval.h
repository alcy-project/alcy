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

// The data an emitter needs to resolve checked tables. The resolvers
// are concrete objects whose bodies call `CompEval` directly: no captured
// lambdas, no hidden evaluator state. `site` changes when a comp callee
// in another module is inlined.
struct EmitContext {
  CompEval& evaluator;
  comp::CompScope& scope;
  EvalSite site;

  struct ExprType {
    const EmitContext& ctx;
    ir::TypeIdx operator()(ast::ExprIdx expr) const;
  };
  struct LayoutType {
    const EmitContext& ctx;
    ir::TypeIdx operator()(ast::ExprIdx expr) const;
  };
  struct PathValue {
    const EmitContext& ctx;
    bool operator()(ast::PathIdx path, comp::CompVal& out) const;
  };
  struct CompFn {
    const EmitContext& ctx;
    bool operator()(ast::ExprIdx call, CompFnInfo& info) const;
  };
  struct FieldIndex {
    const EmitContext& ctx;
    bool operator()(ir::TypeIdx type, std::string_view name, u32& index) const;
  };
  struct VariantIndex {
    const EmitContext& ctx;
    bool operator()(ir::TypeIdx type, std::string_view name, u32& index) const;
  };

  ExprType expr_type{*this};
  LayoutType resolve_layout_type{*this};
  PathValue resolve_path{*this};
  CompFn resolve_comp_fn{*this};
  FieldIndex resolve_field_index{*this};
  VariantIndex resolve_variant_index{*this};

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
