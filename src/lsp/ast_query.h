// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>

#include "ast/ast.h"
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

}  // namespace lsp
