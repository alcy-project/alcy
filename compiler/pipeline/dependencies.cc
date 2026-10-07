// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/dependencies.h"

#include <optional>
#include <string>
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
#include "pkg/arena_copy.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

namespace {

struct ManifestBytes {
  source::FileId file = source::UNKNOWN_FILE;
  std::string_view bytes;
};

// Loads the manifest bytes at `manifest_path`. The file stays
// loaded in the source manager, so syntax errors carry a span.
base::Result<ManifestBytes, diag::Reported> load_manifest_bytes(
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
  return base::make_ok(ManifestBytes{loaded, *bytes});
}

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
    const path::Path& manifest_path,
    const pkg::SuiteManifest* suite) {
  base::Result<ManifestBytes, diag::Reported> loaded_bytes =
      load_manifest_bytes(ctx, dir, manifest_path);
  if (loaded_bytes.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const ManifestBytes manifest_bytes = std::move(loaded_bytes).unwrap();
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(manifest_bytes.bytes, manifest_path.as_view(),
                          manifest_bytes.file, ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pkg::PackageManifest manifest = std::move(parsed).unwrap();
  // A member takes the identity keys it spelled from its suite before
  // the verifier sees it (ADR-0057).
  if (suite != nullptr) {
    base::Result<void, pkg::InheritError> inherited =
        pkg::inherit_from_suite(manifest, *suite);
    if (inherited.is_err()) {
      pkg::report_inherit_error(std::move(inherited).unwrap_err(),
                                manifest_path.as_view(), ctx.bag);
      return base::make_err(diag::Reported{});
    }
  }
  base::Result<void, pkg::ManifestError> verified =
      pkg::verify_manifest(manifest);
  if (verified.is_err()) {
    pkg::report_manifest_error(std::move(verified).unwrap_err(),
                               manifest_path.as_view(), ctx.bag);
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(manifest);
}

// Reads the manifest of the suite at `dir`, which the caller
// has established names a suite.
base::Result<pkg::SuiteManifest, diag::Reported> read_suite_manifest(
    PipelineContext& ctx,
    const path::Path& dir,
    const path::Path& manifest_path) {
  base::Result<ManifestBytes, diag::Reported> loaded_bytes =
      load_manifest_bytes(ctx, dir, manifest_path);
  if (loaded_bytes.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const ManifestBytes manifest_bytes = std::move(loaded_bytes).unwrap();
  base::Result<pkg::SuiteManifest, diag::Reported> parsed =
      pkg::parse_suite_manifest(manifest_bytes.bytes, manifest_path.as_view(),
                                manifest_bytes.file, ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pkg::SuiteManifest manifest = std::move(parsed).unwrap();
  base::Result<void, pkg::SuiteError> verified =
      pkg::verify_suite_manifest(manifest);
  if (verified.is_err()) {
    pkg::report_suite_error(std::move(verified).unwrap_err(),
                            manifest_path.as_view(), ctx.bag);
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(manifest);
}

bool load_path_edge(PipelineContext& ctx,
                    const path::Path& parent_dir,
                    const pkg::Dependency& dep,
                    std::vector<path::Path>& visited,
                    std::vector<path::Path>& loaded,
                    std::vector<LoadedDependency>& staged);

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
                     std::string_view suite,
                     std::string_view expected_name,
                     const pkg::SuiteManifest* suite_manifest,
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
      read_manifest(ctx, dir, manifest_path, suite_manifest);
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
  // A suite addresses a member by its package name, so the member's
  // own manifest has to carry that name.
  if (!expected_name.empty() && manifest.name != expected_name) {
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineDependencyMemberNameMismatch>(
            diag::Severity::Error, diag::Stage::Pipeline,
            DiagCode::DependencySuiteMismatch, spec, manifest.name,
            expected_name);
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
  dependency.suite = suite;
  dependency.modules = std::move(selection).unwrap();
  dependency.files = std::move(found.files);
  staged.push_back(std::move(dependency));
  loaded.push_back(dir);

  for (u32 i = 0; i < manifest.dependency_count; ++i) {
    if (!load_path_edge(ctx, dir, manifest.dependencies[i], visited, loaded,
                        staged)) {
      return false;
    }
  }
  visited.pop_back();
  return true;
}

// Loads the members `dep` selects from the suite at `dir`: every
// member the suite lists for a glob, or the one member a
// three-segment specifier names. The suite's owner and name must
// be the ones the specifier names, and each member loads as a
// package through its own manifest, behind the visited and loaded
// sets the package loader shares, so cycles and shared members
// behave the same across the suite boundary.
bool load_suite_members(PipelineContext& ctx,
                        const path::Path& dir,
                        const pkg::Dependency& dep,
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
  const path::Path manifest_path = dir.join(pkg::MANIFEST_FILE_NAME);
  base::Result<pkg::SuiteManifest, diag::Reported> read =
      read_suite_manifest(ctx, dir, manifest_path);
  if (read.is_err()) {
    return false;
  }
  const pkg::SuiteManifest suite = std::move(read).unwrap();
  if (suite.owner != dep.owner || suite.name != dep.suite) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineDependencySuiteMismatch>(
        diag::Severity::Error, diag::Stage::Pipeline,
        DiagCode::DependencySuiteMismatch, dep.spec, dep.owner, dep.suite,
        dir.as_view(), suite.owner, suite.name);
    (void)index;
    return false;
  }
  std::vector<std::string_view> members;
  if (dep.suite_glob) {
    members.reserve(suite.package_count);
    for (u32 i = 0; i < suite.package_count; ++i) {
      members.push_back(suite.packages[i]);
    }
  } else {
    // The address names the member's package; the path the suite
    // lists is found by its last segment.
    std::string_view listed;
    for (u32 i = 0; i < suite.package_count; ++i) {
      if (pkg::suite_member_name(suite.packages[i]) == dep.member) {
        listed = suite.packages[i];
        break;
      }
    }
    if (listed.empty()) {
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineDependencyNotASuiteMember>(
              diag::Severity::Error, diag::Stage::Pipeline,
              DiagCode::DependencySuiteMismatch, dep.spec, dep.owner,
              dep.suite);
      (void)index;
      return false;
    }
    members.push_back(listed);
  }
  // The suite's closure contains the suite: a member resolving
  // back into this directory is a cycle, not a second load.
  visited.push_back(dir);
  // The suite identity each member's policy carries: `owner/name`, the
  // same spelling a suite specifier uses.
  std::string suite_identity(dep.owner);
  suite_identity += '/';
  suite_identity += dep.suite;
  const std::string_view member_suite =
      pkg::copy_str(ctx.arena, suite_identity);
  for (std::string_view member : members) {
    // The spec a member's own diagnostics name: the specifier as
    // written for one member, the specifier with the member filled
    // in for a glob.
    std::string member_spec(dep.spec);
    if (dep.suite_glob) {
      member_spec.pop_back();
      member_spec += std::string(member);
    }
    if (!load_dependency(ctx, dir.join(member), member_spec, member_suite,
                         pkg::suite_member_name(member), &suite, visited,
                         loaded, staged)) {
      return false;
    }
  }
  visited.pop_back();
  return true;
}

// Loads one path edge: a package directory straight into the
// loader, a suite directory through its member list. Only a local
// directory loads from source; any other source joins the
// selection the caller merges, which says why it cannot be read.
bool load_path_edge(PipelineContext& ctx,
                    const path::Path& parent_dir,
                    const pkg::Dependency& dep,
                    std::vector<path::Path>& visited,
                    std::vector<path::Path>& loaded,
                    std::vector<LoadedDependency>& staged) {
  if (dep.source != pkg::DependencySource::Path) {
    return true;
  }
  const path::Path dir = parent_dir.join(dep.path);
  if (dep.suite_glob || !dep.suite.empty()) {
    return load_suite_members(ctx, dir, dep, visited, loaded, staged);
  }
  return load_dependency(ctx, dir, dep.spec, {}, {}, nullptr, visited, loaded,
                         staged);
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
    if (!load_path_edge(ctx, root, manifest.dependencies[i], visited, loaded,
                        staged)) {
      return base::make_err(diag::Reported{});
    }
  }
  return base::make_ok(std::move(staged));
}

}  // namespace pipeline
