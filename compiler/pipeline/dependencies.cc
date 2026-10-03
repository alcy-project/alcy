// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/dependencies.h"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
#include "pipeline/modules.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

namespace {

// Whether the name is one module path segment, which is what
// the package root a `use` spells has to be: a name holding
// a separator would stage its modules behind a root of its
// own making rather than the package's.
bool is_module_segment(std::string_view name) {
  return name.find('/') == std::string_view::npos &&
         name.find("::") == std::string_view::npos;
}

// Reads the manifest of the package at `dir`, which the
// caller has established names a package.
base::Result<pkg::PackageManifest, diag::Reported> read_manifest(
    PipelineContext& ctx,
    const path::Path& dir,
    const path::Path& manifest_path) {
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(manifest_path.as_view());
  if (file.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineNoManifest>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoManifest,
        dir.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const source::FileId loaded = std::move(file).unwrap();
  const std::optional<std::string_view> bytes = ctx.sources.bytes(loaded);
  if (!bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNotLoaded>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        manifest_path.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(*bytes, manifest_path.as_view(), loaded, ctx.bag,
                          ctx.arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pkg::PackageManifest manifest = std::move(parsed).unwrap();
  base::Result<void, pkg::ManifestError> verified =
      pkg::verify_manifest(manifest);
  if (verified.is_err()) {
    pkg::report_manifest_error(std::move(verified).unwrap_err(),
                               manifest_path.as_view(), ctx.bag);
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(manifest);
}

// Loads the package at `dir`, the dependency `spec` names,
// and every path dependency its manifest declares. `visited`
// holds the directories being resolved, so a directory that
// names one already on the stack resolves into itself, and
// `loaded` holds the directories already staged, so a
// dependency two packages share stages once, behind the one
// root both edges reach.
bool load_dependency(PipelineContext& ctx,
                     const path::Path& dir,
                     std::string_view spec,
                     std::vector<path::Path>& visited,
                     std::vector<path::Path>& loaded,
                     std::vector<LoadedDependency>& staged) {
  for (const path::Path& seen : visited) {
    if (seen == dir) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineDependencyCycle>(
          diag::Severity::Error, diag::Stage::Pipeline,
          DiagCode::DependencyCycle, dir.as_view());
      (void)index;
      return false;
    }
  }
  for (const path::Path& seen : loaded) {
    if (seen == dir) {
      return true;
    }
  }
  visited.push_back(dir);

  const path::Path manifest_path = dir.join(pkg::MANIFEST_FILE_NAME);
  base::Result<pkg::PackageManifest, diag::Reported> read =
      read_manifest(ctx, dir, manifest_path);
  if (read.is_err()) {
    return false;
  }
  const pkg::PackageManifest manifest = std::move(read).unwrap();
  // A dependency is a package, and a package says which files
  // it is: one that declares no target is the error a package
  // without one is, not a package the loader reads anyway.
  if (manifest.bin_count == 0 && manifest.lib == nullptr) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNoTarget>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets,
        manifest.name);
    (void)index;
    return false;
  }
  if (!is_module_segment(manifest.name)) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineDependencyBadName>(
        diag::Severity::Error, diag::Stage::Pipeline,
        DiagCode::DependencyBadName, spec, manifest.name);
    (void)index;
    return false;
  }

  base::Result<pipeline::DiscoveredSources, diag::Reported> discovered =
      pipeline::discover_sources(dir.as_view(), ctx.sources, ctx.bag);
  if (discovered.is_err()) {
    return false;
  }
  pipeline::DiscoveredSources found = std::move(discovered).unwrap();
  base::Result<std::vector<analyzer::ModuleInput>, diag::Reported> selection =
      select_modules(ctx, manifest, dir, found.files);
  if (selection.is_err() || ctx.bag.has_errors()) {
    return false;
  }

  LoadedDependency dependency;
  dependency.manifest = manifest;
  dependency.modules = std::move(selection).unwrap();
  dependency.files = std::move(found.files);
  staged.push_back(std::move(dependency));
  loaded.push_back(dir);

  for (u32 i = 0; i < manifest.dependency_count; ++i) {
    const pkg::Dependency& dep = manifest.dependencies[i];
    // Only a local directory loads from source; a dependency
    // that names another kind joins the selection the caller
    // merges, which says why it cannot be read.
    if (dep.source != pkg::DependencySource::Path) {
      continue;
    }
    if (!load_dependency(ctx, dir.join(dep.path), dep.spec, visited, loaded,
                         staged)) {
      return false;
    }
  }
  visited.pop_back();
  return true;
}

}  // namespace

base::Result<std::vector<LoadedDependency>, diag::Reported>
resolve_dependencies(PipelineContext& ctx,
                     const path::Path& root,
                     const pkg::PackageManifest& manifest) {
  std::vector<LoadedDependency> staged;
  std::vector<path::Path> visited;
  std::vector<path::Path> loaded;
  visited.push_back(root);
  loaded.push_back(root);
  for (u32 i = 0; i < manifest.dependency_count; ++i) {
    const pkg::Dependency& dep = manifest.dependencies[i];
    if (dep.source != pkg::DependencySource::Path) {
      continue;
    }
    if (!load_dependency(ctx, root.join(dep.path), dep.spec, visited, loaded,
                         staged)) {
      return base::make_err(diag::Reported{});
    }
  }
  return base::make_ok(std::move(staged));
}

}  // namespace pipeline
