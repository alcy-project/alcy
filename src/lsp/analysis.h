// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "i18n/language.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "lowering/lowering.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "lsp/workspace.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

// One checked reading of a package: the pipeline context that owns every
// view, one checked package per artifact, and the files each one came
// from. An editor request holds this through a shared pointer and reads a
// single document's package as a `DocumentAnalysis` view.
//
// The tree comes from the compiler's own package entry points, so an
// editor answer and a command-line answer cannot come from two readings
// of the source. A buffer without a manifest, or one that is a staged
// standard-library source, keeps the single-file entry instead.
struct PackageAnalysis {
  explicit PackageAnalysis(i18n::Language language) : context(language) {}

  pipeline::PipelineContext context;
  std::vector<analyzer::CheckedPackage> checked;
  // The lowered form aligned with `checked`: lowering owns the analyzed
  // type tables, so a feature that asks for a type reads them through
  // this storage rather than a cloned checked package.
  std::vector<std::optional<lowering::LoweredPackage>> lowered;
  // Type tables recovered from a failed lowering, aligned with `checked`.
  // Presence means lowering ran and failed; absence means it did not run.
  std::vector<std::optional<ir::StorageState>> recovered;
  // Source file to the checked package that holds it.
  std::unordered_map<source::FileId, usize> package_of;
  // Open buffer URI to the source file registered for its text.
  std::unordered_map<std::string, source::FileId> files_by_uri;
  path::Path root = path::Path::from_native(".").unwrap();
  std::string fingerprint;
  PositionEncoding encoding = PositionEncoding::Utf16;
  bool ok = false;
};

// Builds the analysis for `focus_uri`, reading the other buffers' text
// instead of the files on disk where they are open. A package root is
// found by walking up from the buffer's path; without one the analysis is
// the single-file reading the editor has always used.
std::unique_ptr<PackageAnalysis> build_analysis(
    std::span<const workspace::BufferView> buffers,
    std::string_view focus_uri,
    i18n::Language language,
    PositionEncoding encoding = PositionEncoding::Utf16);

// One document's view into a package analysis. The public shape is what
// the semantic features read; how the tree was assembled is private to
// the owner. Views stay valid while their owner is held.
class DocumentAnalysis {
 public:
  explicit DocumentAnalysis(const Document& document);
  DocumentAnalysis(std::shared_ptr<PackageAnalysis> owner,
                   source::FileId root,
                   usize package_index);

  DocumentAnalysis(const DocumentAnalysis&) = delete;
  DocumentAnalysis& operator=(const DocumentAnalysis&) = delete;
  DocumentAnalysis(DocumentAnalysis&&) = delete;
  DocumentAnalysis& operator=(DocumentAnalysis&&) = delete;

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] source::FileId root() const { return root_; }
  [[nodiscard]] const pipeline::PipelineContext& context() const {
    return owner_->context;
  }
  [[nodiscard]] const analyzer::CheckedPackage& package() const {
    return *package_;
  }
  [[nodiscard]] const PackageAnalysis& owner() const { return *owner_; }

  // The type tables this document's names resolve against: the lowered
  // result first, then tables recovered from a failed lowering, then the
  // checked package when lowering never ran.
  [[nodiscard]] const ir::StorageState& types() const;

 private:
  std::shared_ptr<PackageAnalysis> owner_;
  const analyzer::CheckedPackage* package_ = nullptr;
  source::FileId root_ = source::UNKNOWN_FILE;
  usize package_index_ = 0;
  bool ok_ = false;
};

// The URI an open buffer registered for a source file, when there is one.
// Toolchain sources have names but no editor URI and answer nothing.
std::optional<std::string> uri_of_file(const DocumentAnalysis& analysis,
                                       source::FileId file);

// The source file an editor URI names, when the analysis read it.
std::optional<source::FileId> file_of_uri(const DocumentAnalysis& analysis,
                                          std::string_view uri);

// A file an editor can address: its URI and the bytes the analysis read.
// Toolchain sources without an editor URI answer nothing.
struct SourceText {
  std::string uri;
  std::string_view text;
};
std::optional<SourceText> source_text_of(const DocumentAnalysis& analysis,
                                         source::FileId file);

// A location resolved from the analysis's own source text, or nothing
// when the file has no editor URI or the bytes were not loaded.
std::optional<text::JsonValue> location_json(const DocumentAnalysis& analysis,
                                             source::FileId file,
                                             diag::Span span,
                                             PositionEncoding encoding,
                                             PositionCache* cache);

// The name written at a function's `args: ..` slot, or empty when the
// declaration has no variadic parameter or is a synthesized signature.
std::string_view variadic_param_name(const pipeline::PipelineContext& context,
                                     ast::ItemIdx item);

// The type the checker recorded for an expression, if it recorded one.
// An entry exists once per instantiation; the declaration's own entry is
// the one a query without a call site wants.
std::optional<ir::TypeIdx> checked_expr_type(
    const analyzer::CheckedPackage& package,
    ast::ExprIdx expr);

}  // namespace lsp
