// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/target.h"

#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "app/driver_context.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline.h"
#include "pkg/manifest.h"
#include "pkg/modules.h"
#include "source/source.h"

namespace app {

diag::Fallible<BinTarget> resolve_bin_target(
    DriverContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest,
    std::string_view manifest_name) {
  if (manifest.bin_count == 0) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverNoTargets,
                                   "manifest '{}' declares no [[bin]] targets",
                                   manifest_name);
    (void)index;
    return base::make_err(diag::Fatal{});
  }
  if (manifest.bin_count > 1) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, kDriverNoTargets,
        "manifest '{}' declares {} [[bin]] targets; only one is supported",
        manifest_name, manifest.bin_count);
    (void)index;
    return base::make_err(diag::Fatal{});
  }

  diag::Fallible<pipeline::DiscoveredSources> discovered =
      pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);
  if (discovered.is_err()) {
    return base::make_err(diag::Fatal{});
  }
  pipeline::DiscoveredSources found = std::move(discovered).unwrap();
  const std::vector<source::FileId>& files = found.files;
  const path::Path bin_path = root.join(manifest.bins[0].path);
  source::FileId bin_file = source::kUnknownFile;
  for (source::FileId id : files) {
    if (ctx.sources.name(id) == bin_path.as_view()) {
      bin_file = id;
      break;
    }
  }
  if (bin_file == source::kUnknownFile) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverNoTargets,
                                   "bin target '{}' was not discovered",
                                   manifest.bins[0].path);
    (void)index;
    return base::make_err(diag::Fatal{});
  }

  bool bin_selected = false;
  diag::Fallible<std::vector<pkg::ModuleFile>> selection =
      pkg::resolve_module_files(manifest, root.as_view(), files, ctx.sources,
                                ctx.bag, ctx.arena);
  if (selection.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Fatal{});
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
    } else {
      std::string_view name = entry.name;
      if (!bin_dir.empty() && name.size() > bin_dir.size() &&
          name.substr(0, bin_dir.size()) == bin_dir &&
          name[bin_dir.size()] == '/') {
        name.remove_prefix(bin_dir.size() + 1);
      }
      inputs.push_back({name, entry.id});
    }
  }
  if (!bin_selected) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverNoTargets,
                                   "bin target '{}' is not in [modules]",
                                   manifest.bins[0].path);
    (void)index;
    return base::make_err(diag::Fatal{});
  }

  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      bin_file, inputs, manifest.name, ctx.sources, ctx.arena, ctx.bag);
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Fatal{});
  }
  BinTarget target;
  target.tree = std::move(tree).unwrap();
  target.file_count = files.size();
  target.bin_name = manifest.bins[0].name;
  return base::make_ok(target);
}

}  // namespace app
