// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>

#include "ast/ast.h"
#include "diag/span.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace lsp {

// The name span of a named item declaration, or the item's whole span
// when no single name locates it.
diag::Span item_name_span(const ast::AstArena& ast, const ast::ItemNode& item);

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

}  // namespace lsp
