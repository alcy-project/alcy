// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/target.h"

#include <cstdlib>
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
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "path/path.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_stage.h"
#include "pkg/manifest.h"
#include "pkg/modules.h"
#include "pkg/toolchain.h"
#include "source/source.h"

namespace pipeline {

base::Result<ManifestProbe, path::PathError> find_package_manifest(
    PipelineContext& ctx,
    std::string_view raw) {
  base::Result<path::Path, path::PathError> dir = path::Path::from_native(raw);
  if (dir.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "invalid target '{}'", raw);
    (void)index;
    return base::make_err(std::move(dir).unwrap_err());
  }
  path::Path root = std::move(dir).unwrap();
  const path::Path manifest_path = root.join(pkg::MANIFEST_FILE_NAME);
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_err()) {
    return base::make_ok(
        ManifestProbe{false, std::move(root), source::UNKNOWN_FILE, {}});
  }
  const source::FileId loaded = std::move(manifest).unwrap();
  return base::make_ok(ManifestProbe{true, std::move(root), loaded,
                                     std::string(manifest_path.as_view())});
}

base::Result<BinTarget, diag::Reported> resolve_package_target(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name) {
  // Entry validation: the caller supplies the raw manifest id, so it
  // must name a loaded file before anything parses it.
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(manifest_file);
  if (!manifest_bytes.has_value()) {
    const u32 index =
        ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                     "manifest '{}' is not a loaded file", manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(*manifest_bytes, manifest_name, manifest_file,
                          ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const pkg::PackageManifest manifest = std::move(parsed).unwrap();
  return resolve_bin_target(ctx, root, manifest, manifest_name);
}

base::Result<BinTarget, diag::Reported> resolve_bin_target(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest,
    std::string_view manifest_name) {
  // Entry validation: the manifest crosses in as raw data, so its
  // structure is checked once here before any field is read.
  base::Result<void, pkg::ManifestError> verified =
      pkg::verify_manifest(manifest);
  if (verified.is_err()) {
    pkg::report_manifest_error(std::move(verified).unwrap_err(), manifest_name,
                               ctx.bag);
    return base::make_err(diag::Reported{});
  }
  if (manifest.bin_count == 0) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_NO_TARGETS,
                                   "manifest '{}' declares no [[bin]] targets",
                                   manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (manifest.bin_count > 1) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, PIPELINE_NO_TARGETS,
        "manifest '{}' declares {} [[bin]] targets; only one is supported",
        manifest_name, manifest.bin_count);
    (void)index;
    return base::make_err(diag::Reported{});
  }

  base::Result<pipeline::DiscoveredSources, diag::Reported> discovered =
      pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);
  if (discovered.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pipeline::DiscoveredSources found = std::move(discovered).unwrap();
  const std::vector<source::FileId>& files = found.files;
  const path::Path bin_path = root.join(manifest.bins[0].path);
  source::FileId bin_file = source::UNKNOWN_FILE;
  for (source::FileId id : files) {
    const std::optional<std::string_view> name = ctx.sources.name(id);
    if (name.has_value() && *name == bin_path.as_view()) {
      bin_file = id;
      break;
    }
  }
  if (bin_file == source::UNKNOWN_FILE) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_NO_TARGETS,
                                   "bin target '{}' was not discovered",
                                   manifest.bins[0].path);
    (void)index;
    return base::make_err(diag::Reported{});
  }

  bool bin_selected = false;
  base::Result<std::vector<pkg::ModuleFile>, diag::Reported> selection =
      pkg::resolve_module_files(manifest, root.as_view(), files, ctx.sources,
                                ctx.bag, ctx.arena);
  if (selection.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  std::vector<analyzer::ModuleInput> inputs;
  // Module paths resolve relative to the entry file's directory.
  std::string_view bin_dir;
  {
    const std::string_view bin_path = manifest.bins[0].path;
    const usize slash = bin_path.rfind('/');
    if (slash != std::string_view::npos) {
      bin_dir = bin_path.substr(0, slash);
    }
  }
  for (const pkg::ModuleFile& entry : std::move(selection).unwrap()) {
    if (entry.id == bin_file) {
      bin_selected = true;
      inputs.push_back({"", entry.id});
      continue;
    }
    std::string_view name = entry.name;
    if (!bin_dir.empty() && name.size() > bin_dir.size() &&
        name.substr(0, bin_dir.size()) == bin_dir &&
        name[bin_dir.size()] == '/') {
      name.remove_prefix(bin_dir.size() + 1);
    }
    // Stripping the bin's directory can map two distinct files onto one
    // module name; the resolver would report that as a duplicate, which
    // is true but hides which file collided. Name the pair here.
    for (const analyzer::ModuleInput& prior : inputs) {
      if (prior.id != entry.id && prior.name == name) {
        const u32 index = ctx.bag.emit(
            diag::Severity::Error, PIPELINE_NO_TARGETS, diag::Span{},
            "module '{}' is shared by two files relative to the bin's "
            "directory '{}'",
            name, bin_dir);
        (void)index;
        return base::make_err(diag::Reported{});
      }
    }
    inputs.push_back({name, entry.id});
  }
  if (!bin_selected) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_NO_TARGETS,
                                   "bin target '{}' is not in [modules]",
                                   manifest.bins[0].path);
    (void)index;
    return base::make_err(diag::Reported{});
  }

  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> prelude =
      [&] {
        PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "prelude",
                                                 "frontend");
        return std_prelude(ctx);
      }();
  if (prelude.is_err()) {
    return base::make_err(diag::Reported{});
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "resolve",
                                             "frontend");
    return analyzer::resolve_modules(bin_file, inputs, manifest.name,
                                     ctx.sources, ctx.ast, ctx.bag,
                                     std::move(prelude).unwrap());
  }();
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  BinTarget target;
  target.tree = std::move(tree).unwrap();
  target.file_count = files.size();
  target.bin_name = manifest.bins[0].name;
  return base::make_ok(target);
}

base::Result<pkg::Toolchain, diag::Reported> load_toolchain(
    PipelineContext& ctx,
    const path::Path& root) {
  const path::Path path =
      root.join(pkg::CONFIG_DIR_NAME).join(pkg::TOOLCHAIN_FILE_NAME);
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(path.as_view());
  if (file.is_err()) {
    return base::make_ok(pkg::Toolchain{});
  }
  const source::FileId loaded = std::move(file).unwrap();
  const std::optional<std::string_view> bytes = ctx.sources.bytes(loaded);
  if (!bytes.has_value()) {
    const u32 index =
        ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                     "toolchain '{}' is not a loaded file", path.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return pkg::parse_toolchain(*bytes, path.as_view(), loaded, ctx.bag,
                              ctx.arena);
}

}  // namespace pipeline
