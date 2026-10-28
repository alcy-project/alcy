// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "ir/common.h"
#include "lsp/document.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace lsp {

// One checked reading of a buffer: the frontend's context, the checked
// package it produced, and the file the buffer was added as. The package
// borrows the context, so the two live and die together and neither
// moves; a feature reads them through the reference.
//
// This is the one entry a semantic feature uses. It runs the compiler's
// own path (front_end_root then analyze_tree), so an editor answer and a
// command-line answer cannot come from two readings of the source.
class DocumentAnalysis {
 public:
  explicit DocumentAnalysis(const Document& document);

  DocumentAnalysis(const DocumentAnalysis&) = delete;
  DocumentAnalysis& operator=(const DocumentAnalysis&) = delete;
  DocumentAnalysis(DocumentAnalysis&&) = delete;
  DocumentAnalysis& operator=(DocumentAnalysis&&) = delete;

  // Whether the frontend produced a checked package.
  [[nodiscard]] bool ok() const { return ok_; }

  // The file the buffer was added as. The package also holds the
  // standard library, whose spans belong to other files; a feature
  // keeps a span only when its file is this one.
  [[nodiscard]] source::FileId root() const { return root_; }

  [[nodiscard]] const pipeline::PipelineContext& context() const {
    return context_;
  }

  [[nodiscard]] const analyzer::CheckedPackage& package() const {
    return *package_;
  }

 private:
  pipeline::PipelineContext context_;
  source::FileId root_ = source::UNKNOWN_FILE;
  bool ok_ = false;
  std::unique_ptr<analyzer::CheckedPackage> package_;
};

// The type the checker recorded for an expression, if it recorded one.
// An entry exists once per instantiation; the declaration's own entry is
// the one a query without a call site wants.
std::optional<ir::TypeIdx> checked_expr_type(
    const analyzer::CheckedPackage& package,
    ast::ExprIdx expr);

}  // namespace lsp
