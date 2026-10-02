// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/modules.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
#include "pipeline/pipeline_context.h"
#include "pkg/arena_copy.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

namespace {

// Strips `root` from `path` along with the separator that follows it.
// False when `path` lies outside `root`; the prefix must end on a
// separator boundary, so `/proj` does not match `/project/a.al`.
//
// Paths are canonical - they came from `path::Path`, whose contract is one
// spelling - so the only separator is the canonical one.
bool relative_to(std::string_view path,
                 std::string_view root,
                 std::string_view& out) {
  // A "." root normalizes away: `Path(".").join("a.al")` is `a.al`, so
  // there is no textual prefix left to strip and every discovered path is
  // already root-relative.
  if (root == "." || root.empty()) {
    out = path;
    return true;
  }
  if (path.size() < root.size() || path.substr(0, root.size()) != root) {
    return false;
  }
  std::string_view rest = path.substr(root.size());
  if (!rest.empty() && rest.front() != path::DEFAULT_PATH_SEPARATOR) {
    return false;
  }
  while (!rest.empty() && rest.front() == path::DEFAULT_PATH_SEPARATOR) {
    rest.remove_prefix(1);
  }
  out = rest;
  return true;
}

// The module name a root-relative source path gives: `utils/io` from
// `utils/io.al`. Empty when the path is not a source file, which is a
// file the selection skips rather than an error.
std::string_view module_name_of(mem::Arena& arena, std::string_view relative) {
  if (!path::has_source_extension(relative)) {
    return {};
  }
  const std::string_view name =
      relative.substr(0, relative.size() - path::SOURCE_EXTENSION.size());
  return pkg::copy_str(arena, name);
}

}  // namespace

base::Result<std::vector<analyzer::ModuleInput>, diag::Reported> select_modules(
    PipelineContext& ctx,
    const pkg::PackageManifest& manifest,
    const path::Path& root,
    std::span<const source::FileId> files) {
  // Entry validation: manifests built directly, rather than parsed, must
  // be structurally sound before the selection below reads them. A parsed
  // manifest has already passed the same check, so this costs a walk.
  base::Result<void, pkg::ManifestError> verified =
      pkg::verify_manifest(manifest);
  if (verified.is_err()) {
    pkg::report_manifest_error(std::move(verified).unwrap_err(), root.as_view(),
                               ctx.bag);
    return base::make_err(diag::Reported{});
  }
  std::vector<analyzer::ModuleInput> selected;
  // Dedup by file id, not by name: two spellings of one path resolve to
  // one file, and a name collision between two files is a manifest error
  // rather than a silent drop.
  const auto add_module = [&](std::string_view name,
                              source::FileId id) -> bool {
    for (const analyzer::ModuleInput& prior : selected) {
      if (prior.id == id) {
        return true;
      }
    }
    for (const analyzer::ModuleInput& prior : selected) {
      if (prior.name == name) {
        const u32 index = ctx.bag.emit<i18n::Key::PipelineModuleSelectedTwice>(
            diag::Severity::Error, diag::Stage::Pipeline,
            DiagCode::InvalidModuleSelection, diag::Span{}, name);
        (void)index;
        return false;
      }
    }
    selected.push_back({name, id});
    return true;
  };

  for (u32 i = 0; i < manifest.modules.include_count; ++i) {
    const std::string_view entry = manifest.modules.include[i];
    const path::Path candidate =
        root.join(std::string(entry) + std::string(path::SOURCE_EXTENSION));
    source::FileId found = source::UNKNOWN_FILE;
    for (source::FileId id : files) {
      const std::optional<std::string_view> name = ctx.sources.name(id);
      if (name.has_value() && *name == candidate.as_view()) {
        found = id;
        break;
      }
    }
    if (found == source::UNKNOWN_FILE) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineModuleIncludeHasNoFile>(
          diag::Severity::Error, diag::Stage::Pipeline,
          DiagCode::InvalidModuleSelection, diag::Span{}, entry);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    // The name comes from the resolved path, not the manifest entry, so
    // an entry that only normalizes to a valid spelling (`./util`,
    // `sub//util`) still yields the module the wildcard would give it.
    // Otherwise the two spellings register the same file under different
    // names and it compiles twice.
    std::string_view relative;
    if (!relative_to(candidate.as_view(), root.as_view(), relative)) {
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineModuleIncludeEscapesPackage>(
              diag::Severity::Error, diag::Stage::Pipeline,
              DiagCode::InvalidModuleSelection, diag::Span{}, entry);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    const std::string_view name = module_name_of(ctx.arena, relative);
    if (name.empty()) {
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineModuleIncludeNotAModulePath>(
              diag::Severity::Error, diag::Stage::Pipeline,
              DiagCode::InvalidModuleSelection, diag::Span{}, entry);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    if (!add_module(name, found)) {
      return base::make_err(diag::Reported{});
    }
  }

  if (manifest.modules.wildcard) {
    for (source::FileId id : files) {
      const std::optional<std::string_view> name = ctx.sources.name(id);
      if (!name.has_value()) {
        continue;
      }
      std::string_view relative;
      if (!relative_to(*name, root.as_view(), relative) || relative.empty()) {
        continue;
      }
      const std::string_view module = module_name_of(ctx.arena, relative);
      if (module.empty()) {
        continue;
      }
      if (!add_module(module, id)) {
        return base::make_err(diag::Reported{});
      }
    }
  }
  for (source::FileId id : files) {
    bool taken = false;
    for (const analyzer::ModuleInput& entry : selected) {
      if (entry.id == id) {
        taken = true;
        break;
      }
    }
    if (!taken) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineModuleNotSelected>(
          diag::Severity::Warning, diag::Stage::Pipeline,
          DiagCode::UnselectedFile,
          ctx.sources.name(id).value_or(std::string_view{"[unknown file]"}));
      (void)index;
    }
  }
  return base::make_ok(std::move(selected));
}

}  // namespace pipeline
