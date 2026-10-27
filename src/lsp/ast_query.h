// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ast/ast.h"
#include "diag/span.h"

namespace lsp {

// The name span of a named item declaration, or the item's whole span
// when no single name locates it.
diag::Span item_name_span(const ast::AstArena& ast, const ast::ItemNode& item);

}  // namespace lsp
