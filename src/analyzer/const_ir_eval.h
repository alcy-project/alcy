// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "comp/comp_value.h"
#include "comp/ir_interpreter.h"
#include "diag/span.h"
#include "ir/common.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

// The checked shape of a `comp fn` call's callee, for emitting a
// synthetic IR function the interpreter can call.
struct CompFnInfo {
  ast::BlockIdx body = ast::BlockIdx::invalid();
  std::vector<ast::PatternIdx> params;
  std::vector<ir::TypeIdx> param_types;
  ir::TypeIdx ret = ir::TypeIdx(base::INVALID_IDX);
  // A variadic comp fn binds its trailing arguments as a shape; the
  // fixed params bind normally.
  bool variadic = false;
  // The declared element type of a typed variadic, invalid when untyped.
  ir::TypeIdx element = ir::TypeIdx(base::INVALID_IDX);
  // A method call's receiver occupies the first parameter (`self`).
  bool is_method = false;
  // The module and instantiation the callee body was checked under.
  u32 module = 0;
  u32 inst = 0;
};

// The checked tables and resolution callbacks shared by every emitter
// in one synthetic function. Bundled so the recursive emitters do not
// thread the same six callbacks through each call.
struct EmitContext {
  std::function<ir::TypeIdx(ast::ExprIdx)> expr_type;
  std::function<bool(ast::PathIdx, comp::CompVal&)> resolve_path;
  std::function<ir::TypeIdx(ast::ExprIdx)> resolve_layout_type;
  // Fills `callee` with callbacks bound to the callee's module and
  // instantiation; the caller keeps its own context for arguments.
  std::function<bool(ast::ExprIdx, CompFnInfo&, EmitContext&)> resolve_comp_fn;
  std::function<bool(ir::TypeIdx, std::string_view, u32&)> resolve_field_index;
  std::function<bool(ir::TypeIdx, std::string_view, u32&)>
      resolve_variant_index;
};

// Evaluates a checked constant expression through the IR interpreter,
// building a synthetic function in `builder`. Returns false without a
// value for any shape it does not carry and records the reason in
// `*error`. `type` is the checked type of the initializer,
// `expr_type` resolves a subexpression's checked type, and `resolve_path`
// resolves a single-segment path naming another const, and
// `resolve_layout_type` returns the measured type of a size_of/align_of
// call.
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
