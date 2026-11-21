// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>

#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace lsp {

// The smallest expression, or type, whose span holds the offset in the
// root file: the node the position is written inside, not the statement
// around it.
std::optional<ast::ExprIdx> innermost_expr(
    const pipeline::PipelineContext& context,
    usize offset,
    source::FileId root);

std::optional<ast::TypeIdx> innermost_type(
    const pipeline::PipelineContext& context,
    usize offset,
    source::FileId root);

// The written or inferred type of a local binding whose name the offset
// names (an Ident or MutIdent pattern of a declaration statement).
struct BindingType {
  diag::Span written = {};
  ast::ExprIdx init = ast::ExprIdx::invalid();
};
std::optional<BindingType> binding_type_at(
    const pipeline::PipelineContext& context,
    usize offset,
    source::FileId root);

}  // namespace lsp
