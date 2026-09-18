// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "pkg/resolve.h"

#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "path/path.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

namespace {

// Diagnostic codes 2100-2199 are reserved for dependency resolution.
constexpr u32 kResolveIoError = 2100;
constexpr u32 kResolveCycleError = 2101;

diag::Fallible<std::vector<ResolvedPackage>> resolve_into(
    const path::Path& canonical_dir,
    source::SourceManager& sources,
    mem::Arena& arena,
    diag::DiagBag& bag,
    std::vector<path::Path>& visited) {
  for (const path::Path& seen : visited) {
    if (seen == canonical_dir) {
      const u32 index = bag.emit(diag::Severity::Error, kResolveCycleError,
                                 "dependency cycle detected at '{}'",
                                 canonical_dir.as_view());
      (void)index;
      return base::make_err(diag::Fatal{});
    }
  }
  visited.push_back(canonical_dir);

  const path::Path manifest_path = canonical_dir.join(kManifestFileName);
  base::Result<source::FileId, source::SourceError> loaded =
      sources.load(manifest_path.as_view());
  if (loaded.is_err()) {
    const u32 index =
        bag.emit(diag::Severity::Error, kResolveIoError,
                 "cannot read manifest '{}'", manifest_path.as_view());
    (void)index;
    return base::make_err(diag::Fatal{});
  }
  const source::FileId file = std::move(loaded).unwrap();

  diag::Fallible<PackageManifest> parsed = parse_manifest(
      sources.bytes(file), manifest_path.as_view(), file, bag, arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Fatal{});
  }

  std::vector<ResolvedPackage> resolved;
  resolved.push_back(ResolvedPackage{
      .manifest = std::move(parsed).unwrap(),
      .dir = canonical_dir,
      .manifest_file = file,
  });

  // The dependency array lives in the arena and stays put across vector
  // reallocations below; only the count and pointer are hoisted.
  const Dependency* const root_deps = resolved.front().manifest.dependencies;
  const u32 root_dep_count = resolved.front().manifest.dependency_count;
  for (u32 i = 0; i < root_dep_count; ++i) {
    const Dependency& dep = root_deps[i];
    diag::Fallible<std::vector<ResolvedPackage>> child = resolve_into(
        canonical_dir.join(dep.path), sources, arena, bag, visited);
    if (child.is_err()) {
      return base::make_err(diag::Fatal{});
    }
    for (ResolvedPackage& package : std::move(child).unwrap()) {
      resolved.push_back(std::move(package));
    }
  }

  visited.pop_back();
  return base::make_ok(std::move(resolved));
}

}  // namespace

diag::Fallible<std::vector<ResolvedPackage>> resolve_package(
    std::string_view dir,
    source::SourceManager& sources,
    mem::Arena& arena,
    diag::DiagBag& bag) {
  base::Result<path::Path, path::PathError> canonical =
      path::Path::from_native(dir);
  if (canonical.is_err()) {
    const u32 index = bag.emit(diag::Severity::Error, kResolveIoError,
                               "invalid package directory '{}'", dir);
    (void)index;
    return base::make_err(diag::Fatal{});
  }
  std::vector<path::Path> visited;
  return resolve_into(std::move(canonical).unwrap(), sources, arena, bag,
                      visited);
}

}  // namespace pkg
