// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/target.h"
#include "pipeline/diag_code.h"

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
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/embedded_std.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
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
    const u32 index = ctx.bag.emit<i18n::Key::PipelineInvalidTarget>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError, raw);
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

namespace {

// Resolves one target, binary or library, into its module tree. The
// root file becomes the nameless entry module; the remaining selected
// modules resolve around it exactly as for a binary.
base::Result<PackageTarget, diag::Reported> resolve_target(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest,
    std::string_view target_path,
    std::string_view target_name,
    bool is_lib) {
  base::Result<pipeline::DiscoveredSources, diag::Reported> discovered =
      pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);
  if (discovered.is_err()) {
    return base::make_err(diag::Reported{});
  }
  pipeline::DiscoveredSources found = std::move(discovered).unwrap();
  const std::vector<source::FileId>& files = found.files;
  const path::Path target_file_path = root.join(target_path);
  source::FileId root_file = source::UNKNOWN_FILE;
  for (source::FileId id : files) {
    const std::optional<std::string_view> name = ctx.sources.name(id);
    if (name.has_value() && *name == target_file_path.as_view()) {
      root_file = id;
      break;
    }
  }
  // The two rejections below name the kind of target that failed, so a
  // library and a binary in one package are told apart.
  if (root_file == source::UNKNOWN_FILE) {
    const u32 index =
        is_lib ? ctx.bag.emit<i18n::Key::PipelineLibTargetNotDiscovered>(
                     diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets, target_path)
               : ctx.bag.emit<i18n::Key::PipelineBinTargetNotDiscovered>(
                     diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets, target_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }

  bool root_selected = false;
  base::Result<std::vector<pkg::ModuleFile>, diag::Reported> selection =
      pkg::resolve_module_files(manifest, root.as_view(), files, ctx.sources,
                                ctx.bag, ctx.arena);
  if (selection.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  std::vector<analyzer::ModuleInput> inputs;
  // Module paths resolve relative to the target root's directory.
  std::string_view target_dir;
  {
    const usize slash = target_path.rfind('/');
    if (slash != std::string_view::npos) {
      target_dir = target_path.substr(0, slash);
    }
  }
  for (const pkg::ModuleFile& entry : std::move(selection).unwrap()) {
    if (entry.id == root_file) {
      root_selected = true;
      inputs.push_back({"", entry.id});
      continue;
    }
    std::string_view name = entry.name;
    if (!target_dir.empty() && name.size() > target_dir.size() &&
        name.substr(0, target_dir.size()) == target_dir &&
        name[target_dir.size()] == '/') {
      name.remove_prefix(target_dir.size() + 1);
    }
    // Stripping the target's directory can map two distinct files onto
    // one module name; the resolver would report that as a duplicate,
    // which is true but hides which file collided. Name the pair here.
    for (const analyzer::ModuleInput& prior : inputs) {
      if (prior.id != entry.id && prior.name == name) {
        const u32 index =
            ctx.bag.emit<i18n::Key::PipelineModuleSharedByTwoFiles>(
                diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets, diag::Span{}, name,
                target_dir);
        (void)index;
        return base::make_err(diag::Reported{});
      }
    }
    inputs.push_back({name, entry.id});
  }
  if (!root_selected) {
    const u32 index =
        is_lib ? ctx.bag.emit<i18n::Key::PipelineLibTargetNotSelected>(
                     diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets, target_path)
               : ctx.bag.emit<i18n::Key::PipelineBinTargetNotSelected>(
                     diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets, target_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }

  // The manifest's dependencies select the staged members; anything
  // unselected is absent, not merely out of scope.
  base::Result<StdSelection, diag::Reported> selected = resolve_std_selection(
      {manifest.dependencies, manifest.dependency_count}, ctx.bag);
  if (selected.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  const std::span<const analyzer::ModuleInput> prelude = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "prelude",
                                             "frontend");
    return std_prelude(ctx, std::move(selected).unwrap());
  }();
  base::Result<analyzer::ModuleTree, diag::Reported> tree = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "resolve",
                                             "frontend");
    const std::span<const analyzer::StdHint> hints(STD_HINTS, STD_HINT_COUNT);
    return analyzer::resolve_modules(root_file, inputs, manifest.name,
                                     ctx.sources, ctx.ast, ctx.bag, prelude,
                                     hints);
  }();
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  PackageTarget target;
  target.tree = std::move(tree).unwrap();
  target.file_count = files.size();
  // A target that names nothing is the package, which is what makes a
  // one-file manifest readable.
  target.name = target_name.empty() ? manifest.name : target_name;
  target.is_lib = is_lib;
  return base::make_ok(target);
}

}  // namespace

base::Result<std::vector<PackageTarget>, diag::Reported>
resolve_package_targets(PipelineContext& ctx,
                        const path::Path& root,
                        source::FileId manifest_file,
                        std::string_view manifest_name) {
  // Entry validation: the caller supplies a raw manifest id, which must
  // name a loaded file, and the bytes must then be structurally whole
  // before any field is read.
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(manifest_file);
  if (!manifest_bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNotLoaded>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError, manifest_name);
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
  base::Result<void, pkg::ManifestError> verified =
      pkg::verify_manifest(manifest);
  if (verified.is_err()) {
    pkg::report_manifest_error(std::move(verified).unwrap_err(), manifest_name,
                               ctx.bag);
    return base::make_err(diag::Reported{});
  }
  if (manifest.bin_count == 0 && manifest.lib == nullptr) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNoTarget>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets, manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (manifest.bin_count > 1) {
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineManifestTooManyBinTargets>(
            diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets, manifest_name,
            manifest.bin_count);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  std::vector<PackageTarget> targets;
  // The binary comes first: `alcy run` takes the front target, and a
  // package's headline artifact is its executable.
  const auto add = [&](std::string_view path, std::string_view name,
                       bool is_lib) -> base::Result<void, diag::Reported> {
    base::Result<PackageTarget, diag::Reported> target =
        resolve_target(ctx, root, manifest, path, name, is_lib);
    if (target.is_err() || ctx.bag.has_errors()) {
      return base::make_err(diag::Reported{});
    }
    targets.push_back(std::move(target).unwrap());
    return base::make_ok();
  };
  if (manifest.bin_count == 1) {
    base::Result<void, diag::Reported> added =
        add(manifest.bins[0].path, manifest.bins[0].name, false);
    if (added.is_err()) {
      return base::make_err(diag::Reported{});
    }
  }
  if (manifest.lib != nullptr) {
    base::Result<void, diag::Reported> added =
        add(manifest.lib->path, manifest.lib->name, true);
    if (added.is_err()) {
      return base::make_err(diag::Reported{});
    }
  }
  return base::make_ok(targets);
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
    const u32 index = ctx.bag.emit<i18n::Key::PkgToolchainNotLoaded>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError, path.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return pkg::parse_toolchain(*bytes, path.as_view(), loaded, ctx.bag,
                              ctx.arena);
}

}  // namespace pipeline
