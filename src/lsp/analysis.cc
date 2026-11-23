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
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "linter/config.h"
#include "linter/engine.h"
#include "linter/lint.h"
#include "lowering/lowering.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "lsp/workspace.h"
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "pipeline/std_stage.h"
#include "source/source.h"
#include "text/json.h"

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
                const linter::LintConfig& config) {
  // One reading of the tree feeds both the diagnostics tail and the
  // editor features. Lowering moves the analyzed type tables out, so
  // keep the checked metadata it hands back and the lowered storage
  // that now owns the types - neither is copied.
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      pipeline::analyze_tree(out.context, tree);
  if (checked.is_err()) {
    return;
  }
  std::optional<analyzer::CheckedPackage> features =
      std::move(checked).unwrap();
  std::optional<lowering::LoweredPackage> lowered;
  std::optional<ir::StorageState> recovered;
  ir::StorageState recovered_state;
  if (!out.context.bag.has_errors()) {
    (void)linter::run_lints(out.context, *features, config);
    if (config.borrow_checks) {
      std::optional<analyzer::CheckedPackage> consumed;
      std::optional<lowering::LoweredPackage> lowered_out;
      base::Result<pipeline::FrontendOutput, diag::Reported> finished =
          pipeline::finish_frontend(out.context, std::move(*features),
                                    &consumed, &lowered_out, &recovered_state);
      if (finished.is_ok()) {
        lowered = std::move(std::move(finished).unwrap().package);
      } else if (lowered_out.has_value()) {
        // A borrow error does not invalidate the type tables lowering
        // already built, so editor type answers can still use them.
        lowered = std::move(lowered_out);
      }
      if (!lowered.has_value() && !recovered_state.types.empty()) {
        recovered.emplace(std::move(recovered_state));
      }
      // The call hands the consumed package back on every exit.
      features.swap(consumed);
    }
  }
  if (!features.has_value()) {
    return;
  }
  const usize index = out.checked.size();
  for (const analyzer::ModuleNode* module : features->tree.modules) {
    if (module->file != source::UNKNOWN_FILE) {
      out.package_of[module->file] = index;
    }
  }
  out.checked.push_back(std::move(*features));
  out.lowered.push_back(std::move(lowered));
  out.recovered.push_back(std::move(recovered));
  DCHECK_EQ(out.checked.size(), out.lowered.size());
  DCHECK_EQ(out.checked.size(), out.recovered.size());
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
  check_tree(out, std::move(tree).unwrap(), config);
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
    check_tree(out, artifact.tree, config);
  }
}

}  // namespace

std::unique_ptr<PackageAnalysis> build_analysis(
    std::span<const workspace::BufferView> buffers,
    std::string_view focus_uri,
    i18n::Language language,
    PositionEncoding encoding) {
  auto out = std::make_unique<PackageAnalysis>(language);
  out->encoding = encoding;
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
  package_index_ = package->second;
  package_ = &owner_->checked[package_index_];
  ok_ = true;
}

DocumentAnalysis::DocumentAnalysis(std::shared_ptr<PackageAnalysis> owner,
                                   source::FileId root,
                                   usize package_index)
    : owner_(std::move(owner)), root_(root) {
  if (package_index < owner_->checked.size()) {
    package_index_ = package_index;
    package_ = &owner_->checked[package_index_];
    ok_ = true;
  }
}

const ir::StorageState& DocumentAnalysis::types() const {
  if (package_index_ < owner_->lowered.size()) {
    const auto& lowered = owner_->lowered[package_index_];
    if (lowered.has_value()) {
      return lowered->storage->state();
    }
  }
  if (package_index_ < owner_->recovered.size()) {
    const auto& recovered = owner_->recovered[package_index_];
    if (recovered.has_value()) {
      return *recovered;
    }
  }
  // Reached only when lowering never ran: the checked storage still owns
  // its tables. After lowering ran, they moved out and one of the two
  // entries above is present.
  return package_->types->state();
}

namespace {

std::string_view declared_param_name(const pipeline::PipelineContext& context,
                                     const ast::ItemFnParam& param) {
  const ast::PatternNode& pattern = context.ast.patterns[param.pattern];
  if (pattern.kind == ast::PatternKind::Ident) {
    return pattern.payload.ident.name.name;
  }
  if (pattern.kind == ast::PatternKind::MutIdent) {
    return pattern.payload.mut_ident.name.name;
  }
  return {};
}

std::span<const ast::ItemFnParam> declared_params(
    const pipeline::PipelineContext& context,
    ast::ItemIdx item) {
  if (!item.is_valid()) {
    return {};
  }
  const ast::ItemNode& node = context.ast.items[item];
  if (node.kind == ast::ItemKind::Fn) {
    return node.payload.get<ast::ItemFn>().params;
  }
  if (node.kind == ast::ItemKind::Intrinsic) {
    return node.payload.get<ast::ItemIntrinsic>().params;
  }
  return {};
}

}  // namespace

std::string_view param_name(const pipeline::PipelineContext& context,
                            ast::ItemIdx item,
                            usize index) {
  usize seen = 0;
  for (const ast::ItemFnParam& param : declared_params(context, item)) {
    if (param.is_variadic) {
      continue;
    }
    if (seen++ == index) {
      return declared_param_name(context, param);
    }
  }
  return {};
}

std::string_view variadic_param_name(const pipeline::PipelineContext& context,
                                     ast::ItemIdx item) {
  for (const ast::ItemFnParam& param : declared_params(context, item)) {
    if (param.is_variadic) {
      return declared_param_name(context, param);
    }
  }
  return {};
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

std::optional<source::FileId> file_of_uri(const DocumentAnalysis& analysis,
                                          std::string_view uri) {
  const auto found = analysis.owner().files_by_uri.find(std::string(uri));
  if (found != analysis.owner().files_by_uri.end()) {
    return found->second;
  }
  const std::optional<std::string> path = workspace::uri_to_path(uri);
  if (!path.has_value()) {
    return std::nullopt;
  }
  for (const analyzer::ModuleNode* module : analysis.package().tree.modules) {
    if (module->file == source::UNKNOWN_FILE) {
      continue;
    }
    const std::optional<std::string_view> name =
        analysis.context().sources.name(module->file);
    if (name.has_value() && *name == *path) {
      return module->file;
    }
  }
  return std::nullopt;
}

std::optional<SourceText> source_text_of(const DocumentAnalysis& analysis,
                                         source::FileId file) {
  const std::optional<std::string> uri = uri_of_file(analysis, file);
  if (!uri.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::string_view> text =
      analysis.context().sources.bytes(file);
  if (!text.has_value()) {
    return std::nullopt;
  }
  return SourceText{*uri, *text};
}

std::optional<text::JsonValue> location_json(const DocumentAnalysis& analysis,
                                             source::FileId file,
                                             diag::Span span,
                                             PositionEncoding encoding,
                                             PositionCache* cache) {
  const std::optional<std::string> uri = uri_of_file(analysis, file);
  const std::optional<std::string_view> text =
      analysis.context().sources.bytes(file);
  if (!uri.has_value() || !text.has_value()) {
    return std::nullopt;
  }
  return lsp::location_json(*uri, *text, span, encoding, cache);
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
