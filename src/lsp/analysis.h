// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "ir/common.h"
#include "lsp/document.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace lsp {

// Runs the compiler frontend over one open buffer into context, and
// returns the checked package it borrows. The caller keeps context alive
// for as long as it reads the package.
//
// This is the one entry a semantic feature uses. It is the compiler's
// own path (front_end_root then analyze_tree), so an editor answer and a
// command-line answer cannot come from two readings of the source.
//
// The package also holds the standard library, whose spans belong to
// other files. A caller that compares a span against the cursor passes
// root and keeps only the spans whose file is the buffer it opened.
base::Result<analyzer::CheckedPackage, diag::Reported> analyze_document(
    pipeline::PipelineContext& context,
    const Document& document,
    source::FileId* root = nullptr);

// The type the checker recorded for an expression, if it recorded one.
// An entry exists once per instantiation; the declaration's own entry is
// the one a query without a call site wants.
std::optional<ir::TypeIdx> checked_expr_type(
    const analyzer::CheckedPackage& package,
    ast::ExprIdx expr);

}  // namespace lsp
