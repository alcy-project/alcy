// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/analysis.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "ir/common.h"
#include "linter/config.h"
#include "linter/lint.h"
#include "lsp/document.h"
#include "lsp/workspace.h"
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pipeline/std_stage.h"
#include "source/source.h"

namespace lsp {

namespace {

const workspace::BufferView* find_buffer(
    std::span<const workspace::BufferView> buffers,
    std::string_view uri) {
  for (const workspace::BufferView& buffer : buffers) {
    if (buffer.uri == uri) {
      return &buffer;
    }
  }
  return nullptr;
}

std::unique_ptr<PackageAnalysis> build_single(const Document& document) {
  const workspace::BufferView buffers[] = {
      {document.uri(), document.text(), document.version()}};
  return build_analysis(buffers, document.uri(), i18n::Language::EnUs);
}

void check_tree(PackageAnalysis& out,
                const analyzer::ModuleTree& tree,
                const linter::LintConfig& config,
                usize file_count) {
  // The lint run publishes compiler errors, lint findings, and the
  // lowering and borrow errors in one bag. The checked package the
  // features read is a second, independent reading of the same tree.
  (void)linter::lint_tree(out.context, tree, config, file_count);
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      pipeline::analyze_tree(out.context, tree);
  if (checked.is_err()) {
    return;
  }
  const usize index = out.checked.size();
  analyzer::CheckedPackage package = std::move(checked).unwrap();
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    if (module->file != source::UNKNOWN_FILE) {
      out.package_of[module->file] = index;
    }
  }
  out.checked.push_back(std::move(package));
  out.ok = true;
  out.context.bag.dedup();
}

void analyze_standalone(PackageAnalysis& out,
                        std::span<const workspace::BufferView> buffers,
                        std::string_view focus_uri) {
  const workspace::BufferView* focus = find_buffer(buffers, focus_uri);
  if (focus == nullptr) {
    return;
  }
  const source::FileId file =
      out.context.sources.add_virtual(focus_uri, focus->text);
  out.files_by_uri[std::string(focus_uri)] = file;
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      pipeline::front_end_root(out.context, file,
                               pipeline::full_std_selection());
  if (tree.is_err() || out.context.bag.has_errors()) {
    return;
  }
  linter::LintConfig config{};
  base::Result<linter::LintConfig, diag::Reported> loaded =
      linter::load_config(out.context, out.root);
  if (loaded.is_ok()) {
    config = std::move(loaded).unwrap();
  }
  check_tree(out, std::move(tree).unwrap(), config, 1);
}

void analyze_package(PackageAnalysis& out,
                     std::span<const workspace::BufferView> buffers,
                     std::string_view focus_uri,
                     const path::Path& root) {
  out.root = root;
  for (const workspace::BufferView& buffer : buffers) {
    const std::optional<std::string> path = workspace::uri_to_path(buffer.uri);
    if (!path.has_value() || !workspace::under_root(*path, root.as_view())) {
      continue;
    }
    const source::FileId file =
        out.context.sources.add_virtual(*path, buffer.text);
    out.files_by_uri[std::string(buffer.uri)] = file;
  }
  if (!out.files_by_uri.contains(std::string(focus_uri))) {
    const workspace::BufferView* focus = find_buffer(buffers, focus_uri);
    if (focus == nullptr) {
      return;
    }
    const std::optional<std::string> path = workspace::uri_to_path(focus_uri);
    const std::string name = path.value_or(std::string(focus_uri));
    const source::FileId file =
        out.context.sources.add_virtual(name, focus->text);
    out.files_by_uri[std::string(focus_uri)] = file;
  }

  linter::LintConfig config{};
  base::Result<linter::LintConfig, diag::Reported> loaded =
      linter::load_config(out.context, root);
  if (loaded.is_ok()) {
    config = std::move(loaded).unwrap();
  }

  base::Result<pipeline::ManifestProbe, diag::Reported> probe =
      pipeline::require_package_manifest(out.context, root.as_view(), false);
  if (probe.is_err()) {
    return;
  }
  const pipeline::ManifestProbe manifest = std::move(probe).unwrap();
  base::Result<std::vector<pipeline::Artifact>, diag::Reported> resolved =
      pipeline::resolve_artifacts(out.context, root, manifest.manifest,
                                  manifest.manifest_name,
                                  pipeline::ArtifactScope::All);
  if (resolved.is_err()) {
    return;
  }
  std::vector<pipeline::Artifact> artifacts = std::move(resolved).unwrap();
  for (pipeline::Artifact& artifact : artifacts) {
    check_tree(out, artifact.tree, config, artifact.file_count);
  }
}

}  // namespace

std::unique_ptr<PackageAnalysis> build_analysis(
    std::span<const workspace::BufferView> buffers,
    std::string_view focus_uri,
    i18n::Language language) {
  auto out = std::make_unique<PackageAnalysis>(language);
  const std::optional<std::string> focus_path =
      workspace::uri_to_path(focus_uri);
  const std::optional<path::Path> root =
      focus_path.has_value() ? workspace::find_package_root(*focus_path)
                             : std::optional<path::Path>{};
  const bool staged =
      focus_path.has_value() &&
      pipeline::staged_member(*focus_path, pipeline::full_std_selection())
          .has_value();
  if (root.has_value() && !staged) {
    analyze_package(*out, buffers, focus_uri, *root);
  } else {
    analyze_standalone(*out, buffers, focus_uri);
  }
  return out;
}

DocumentAnalysis::DocumentAnalysis(const Document& document)
    : owner_(build_single(document)) {
  const auto found = owner_->files_by_uri.find(std::string(document.uri()));
  if (found == owner_->files_by_uri.end()) {
    return;
  }
  root_ = found->second;
  const auto package = owner_->package_of.find(root_);
  if (package == owner_->package_of.end()) {
    return;
  }
  package_ = &owner_->checked[package->second];
  ok_ = true;
}

DocumentAnalysis::DocumentAnalysis(std::shared_ptr<PackageAnalysis> owner,
                                   source::FileId root,
                                   usize package_index)
    : owner_(std::move(owner)), root_(root) {
  if (package_index < owner_->checked.size()) {
    package_ = &owner_->checked[package_index];
    ok_ = true;
  }
}

std::optional<std::string> uri_of_file(const DocumentAnalysis& analysis,
                                       source::FileId file) {
  for (const auto& [uri, id] : analysis.owner().files_by_uri) {
    if (id == file) {
      return uri;
    }
  }
  const std::optional<std::string_view> name =
      analysis.context().sources.name(file);
  if (name.has_value()) {
    if (const base::Result<path::Path, path::PathError> parsed =
            path::Path::from_native(*name);
        parsed.is_ok() && parsed.value().is_absolute()) {
      return workspace::path_to_uri(*name);
    }
  }
  return std::nullopt;
}

std::optional<ir::TypeIdx> checked_expr_type(
    const analyzer::CheckedPackage& package,
    ast::ExprIdx expr) {
  for (const analyzer::CheckedModule& module : package.modules) {
    if (const analyzer::CheckedModule::ExprType* entry =
            module.expr_type_of(expr, analyzer::NO_INST)) {
      return entry->type;
    }
  }
  std::optional<ir::TypeIdx> fallback;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::ExprType& entry : module.expr_types) {
      if (entry.expr != expr) {
        continue;
      }
      if (!fallback.has_value()) {
        fallback = entry.type;
      }
    }
  }
  return fallback;
}

}  // namespace lsp
