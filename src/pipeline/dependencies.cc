// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/dependencies.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/summary.h"
#include "base/hash.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/io/io_util.h"
#include "i18n/messages.h"
#include "ir/deserialize.h"
#include "ir/summary.h"
#include "ir/write_input.h"
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
        DiagCode::NoManifest, dir.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const source::FileId loaded = std::move(file).unwrap();
  const std::optional<std::string_view> bytes = ctx.sources.bytes(loaded);
  if (!bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNotLoaded>(
        DiagCode::IoError, manifest_path.as_view());
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

namespace {

// Reads the summary a library build leaves beside its object and spells
// its surface, answering false when there is none, when the sources it
// names have outgrown it, or when it carries nothing a dependent can use
// yet (ADR-0078).
bool load_summary_dependency(PipelineContext& ctx,
                             const path::Path& dir,
                             const pkg::PackageManifest& manifest,
                             LoadedDependency& out) {
  if (manifest.lib == nullptr) {
    return false;
  }
  // A library with no name of its own answers to the package's.
  const std::string_view lib_name =
      manifest.lib->name.empty() ? manifest.name : manifest.lib->name;
  const std::string file_name = std::string(lib_name) + ".summary";
  const path::Path sidecar = dir.join(path::DEFAULT_OUT_DIR).join(file_name);
  const std::string sidecar_path(sidecar.as_view());
  if (!io::is_file(sidecar_path)) {
    return false;
  }
  const std::string bytes = io::read_file(sidecar_path);
  const std::span<const u8> view(reinterpret_cast<const u8*>(bytes.data()),
                                 bytes.size());
  base::Result<ir::LoadedIr, ir::IrLoadError> loaded = ir::deserialize(view);
  if (loaded.is_err()) {
    return false;
  }
  ir::LoadedIr artifact = std::move(loaded).unwrap();
  // The artifact's identity names every file its own build read: when
  // they are all here, recompute the digest and refuse a summary the
  // sources have outgrown. A package shipped without its sources is
  // answered from the summary alone.
  bool verified = true;
  std::vector<base::Digest128> hashes = artifact.file_hashes;
  for (usize i = 0; i < artifact.file_names.size(); ++i) {
    if (!io::is_file(artifact.file_names[i])) {
      verified = false;
      break;
    }
    const std::string source = io::read_file(artifact.file_names[i]);
    const std::span<const u8> source_bytes(
        reinterpret_cast<const u8*>(source.data()), source.size());
    if (i < hashes.size()) {
      hashes[i] = base::hash128(source_bytes);
    }
  }
  if (verified) {
    std::vector<std::string_view> names;
    names.reserve(artifact.file_names.size());
    for (const std::string& name : artifact.file_names) {
      names.push_back(name);
    }
    const ir::FileTable table{.names = names, .hashes = hashes};
    if (ir::identity_digest(table, artifact.width, artifact.compiler_version) !=
        artifact.identity) {
      return false;
    }
  }
  // The dependent's lowering reads this summary, so the bytes that
  // answer for the dependency are part of what its cache entry was
  // built from.
  ctx.summary_digests.push_back(base::hash128(view));
  // The module names the dependent will spell, in row order: the same
  // slash form a `use` writes, without the duplicate separators a
  // dotted path spells.
  out.module_names.clear();
  out.modules.clear();
  for (const ir::SummaryEntry& row : artifact.summary) {
    std::string slash =
        analyzer::summary_module_name(artifact.strings->get(row.path));
    bool seen = false;
    for (const std::string_view name : out.module_names) {
      if (name == slash) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    // The name outlives the tree: it is copied into the interner the
    // module inputs borrow.
    const std::optional<str::StringPoolId> name_id =
        ctx.strings.intern_copied(slash);
    if (!name_id.has_value()) {
      return false;
    }
    out.module_names.push_back(ctx.strings.get(*name_id));
    out.modules.push_back(analyzer::ModuleInput{
        out.module_names.back(), source::UNKNOWN_FILE, false, {}});
  }
  if (out.module_names.empty()) {
    return false;
  }
  out.artifact = std::move(artifact);
  // The object the same build wrote, for the link a build performs; the
  // path is copied into the interner so the target that links it can
  // name it.
  const path::Path object_path =
      dir.join(path::DEFAULT_OUT_DIR).join(std::string(lib_name) + ".o");
  const std::optional<str::StringPoolId> object_id =
      ctx.strings.intern_copied(object_path.as_view());
  if (object_id.has_value()) {
    out.object = ctx.strings.get(*object_id);
  }
  return true;
}

}  // namespace

// One resolution in flight: the directories on its stack, where a repeat
// is a cycle, and the entries it has reached in traversal order. The
// entries live in the run's table, so two resolutions that share a
// dependency share its entry.
struct Resolution {
  explicit Resolution(StagedDependencies& store) : store(store) {}

  StagedDependencies& store;
  std::vector<path::Path> visited;
  std::vector<const LoadedDependency*> entries;

  void add(const LoadedDependency* entry) {
    for (const LoadedDependency* seen : entries) {
      if (seen == entry) {
        return;
      }
    }
    entries.push_back(entry);
  }
};

bool load_path_edge(PipelineContext& ctx,
                    Resolution& resolution,
                    const path::Path& parent_dir,
                    const pkg::Dependency& dep,
                    std::vector<const LoadedDependency*>& contributed);

// Loads the package at `dir`, the dependency `spec` names, and every path
// dependency its manifest declares. A directory the run already staged is
// taken from the run's table, subtree and all, so a suite's members do not
// read their shared dependencies again; `contributed` collects what this
// edge brought, which is what a later resolution takes on a hit.

bool load_dependency(PipelineContext& ctx,
                     Resolution& resolution,
                     const path::Path& dir,
                     std::string_view spec,
                     std::string_view suite,
                     std::string_view expected_name,
                     const pkg::SuiteManifest* suite_manifest,
                     std::vector<const LoadedDependency*>& contributed) {
  StagedDependencies& store = resolution.store;
  const std::string key(dir.as_view());
  // A directory the run already staged is taken from the run's table,
  // subtree and all: a suite's members do not read their shared
  // dependencies again. The staged entry answers to the suite its
  // first edge spelled, and the table is keyed by the directory alone,
  // so an edge that spells another suite - a plain path where a suite
  // member was staged, or the reverse - would leave one identity
  // answering for two. A directory is one package; the closure names
  // it one way.
  if (const auto found = store.by_dir.find(key); found != store.by_dir.end()) {
    if (found->second.front()->suite != suite) {
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineDependencySuiteAmbiguous>(
              DiagCode::DependencySuiteMismatch, dir.as_view(),
              found->second.front()->suite, suite);
      (void)index;
      return false;
    }
    for (const LoadedDependency* entry : found->second) {
      resolution.add(entry);
      contributed.push_back(entry);
    }
    return true;
  }
  for (const path::Path& seen : resolution.visited) {
    if (seen == dir) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineDependencyCycle>(
          DiagCode::DependencyCycle, dir.as_view());
      (void)index;
      return false;
    }
  }
  resolution.visited.push_back(dir);

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
        DiagCode::NoTargets, manifest.name);
    (void)index;
    return false;
  }
  if (!is_module_segment(manifest.name)) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineDependencyBadName>(
        DiagCode::DependencyBadName, spec, manifest.name);
    (void)index;
    return false;
  }
  // A suite addresses a member by its package name, so the member's
  // own manifest has to carry that name.
  if (!expected_name.empty() && manifest.name != expected_name) {
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineDependencyMemberNameMismatch>(
            DiagCode::DependencySuiteMismatch, spec, manifest.name,
            expected_name);
    (void)index;
    return false;
  }

  LoadedDependency dependency;
  dependency.manifest = manifest;
  dependency.suite = suite;
  // A fresh summary answers for the package's sources; a package the
  // build left one for is not read at all.
  if (!load_summary_dependency(ctx, dir, manifest, dependency)) {
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
    dependency.modules = std::move(selection).unwrap();
    dependency.files = std::move(found.files);
  }
  // The entry's address is what a closure holds, so the table it lands in
  // never moves its elements.
  store.entries.push_back(std::move(dependency));
  const LoadedDependency* self = &store.entries.back();
  resolution.add(self);
  // What this directory contributes to a closure: itself, and what its own
  // dependencies bring.
  std::vector<const LoadedDependency*> subtree{self};
  for (u32 i = 0; i < manifest.dependency_count; ++i) {
    if (!load_path_edge(ctx, resolution, dir, manifest.dependencies[i],
                        subtree)) {
      return false;
    }
  }
  contributed.insert(contributed.end(), subtree.begin(), subtree.end());
  store.by_dir.emplace(key, std::move(subtree));
  resolution.visited.pop_back();
  return true;
}

// Loads the members `dep` selects from the suite at `dir`: every
// member the suite lists for a glob, or the one member a
// three-segment specifier names. The suite's owner and name must
// be the ones the specifier names, and each member loads as a
// package through its own manifest, behind the visited set and the
// staged-dependency table the package loader shares, so cycles and
// shared members behave the same across the suite boundary.
bool load_suite_members(PipelineContext& ctx,
                        Resolution& resolution,
                        const path::Path& dir,
                        const pkg::Dependency& dep,
                        std::vector<const LoadedDependency*>& contributed) {
  for (const path::Path& seen : resolution.visited) {
    if (seen == dir) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineDependencyCycle>(
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
              DiagCode::DependencySuiteMismatch, dep.spec, dep.owner,
              dep.suite);
      (void)index;
      return false;
    }
    members.push_back(listed);
  }
  // The suite's closure contains the suite: a member resolving
  // back into this directory is a cycle, not a second load.
  resolution.visited.push_back(dir);
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
    if (!load_dependency(ctx, resolution, dir.join(member), member_spec,
                         member_suite, pkg::suite_member_name(member), &suite,
                         contributed)) {
      return false;
    }
  }
  resolution.visited.pop_back();
  return true;
}

// Loads one path edge: a package directory straight into the
// loader, a suite directory through its member list. Only a local
// directory loads from source; any other source joins the
// selection the caller merges, which says why it cannot be read.
bool load_path_edge(PipelineContext& ctx,
                    Resolution& resolution,
                    const path::Path& parent_dir,
                    const pkg::Dependency& dep,
                    std::vector<const LoadedDependency*>& contributed) {
  if (dep.source != pkg::DependencySource::Path) {
    return true;
  }
  const path::Path dir = parent_dir.join(dep.path);
  if (dep.suite_glob || !dep.suite.empty()) {
    return load_suite_members(ctx, resolution, dir, dep, contributed);
  }
  return load_dependency(ctx, resolution, dir, dep.spec, {}, {}, nullptr,
                         contributed);
}

// The resolution itself: it walks the tables and the run's staged
// dependencies, and `resolve_dependencies` below is the entry a caller
// names.
base::Result<DependencyClosure, diag::Reported> resolve_closure(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "dependencies",
                                           "frontend");
  Resolution resolution{ctx.staged_dependencies()};
  resolution.visited.push_back(root);
  std::vector<const LoadedDependency*> contributed;
  for (u32 i = 0; i < manifest.dependency_count; ++i) {
    if (!load_path_edge(ctx, resolution, root, manifest.dependencies[i],
                        contributed)) {
      return base::make_err(diag::Reported{});
    }
  }
  return base::make_ok(DependencyClosure(std::move(resolution.entries)));
}

}  // namespace

base::Result<DependencyClosure, diag::Reported> resolve_dependencies(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest) {
  return resolve_closure(ctx, root, manifest);
}

}  // namespace pipeline
