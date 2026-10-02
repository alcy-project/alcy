// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/resolve.h"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pkg/diag_code.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

namespace {

base::Result<std::vector<ResolvedPackage>, diag::Reported> resolve_into(
    const path::Path& canonical_dir,
    source::SourceManager& sources,
    mem::Arena& arena,
    diag::DiagBag& bag,
    std::vector<path::Path>& visited) {
  for (const path::Path& seen : visited) {
    if (seen == canonical_dir) {
      const u32 index = bag.emit<i18n::Key::PkgDependencyCycle>(
          diag::Severity::Error, diag::Stage::Pkg, DiagCode::ResolveCycleError,
          canonical_dir.as_view());
      (void)index;
      return base::make_err(diag::Reported{});
    }
  }
  visited.push_back(canonical_dir);

  const path::Path manifest_path = canonical_dir.join(MANIFEST_FILE_NAME);
  base::Result<source::FileId, source::SourceError> loaded =
      sources.load(manifest_path.as_view());
  if (loaded.is_err()) {
    const u32 index = bag.emit<i18n::Key::PkgCannotReadManifest>(
        diag::Severity::Error, diag::Stage::Pkg, DiagCode::ResolveIoError,
        manifest_path.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const source::FileId file = std::move(loaded).unwrap();

  // The id came from a successful load above: the bytes are known.
  const std::optional<std::string_view> manifest_bytes = sources.bytes(file);
  DCHECK(manifest_bytes.has_value());
  base::Result<PackageManifest, diag::Reported> parsed =
      parse_manifest(manifest_bytes.value_or(std::string_view{}),
                     manifest_path.as_view(), file, bag, arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
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
    if (dep.source != DependencySource::Path) {
      // Only local directories resolve by inclusion; registry and git
      // sources have no fetcher yet, and embedded entries never reach
      // a path walk.
      const u32 index = bag.emit<i18n::Key::PipelineDependencyNeedsFetcher>(
          diag::Severity::Error, diag::Stage::Pkg, DiagCode::ResolveIoError,
          dep.spec);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    base::Result<std::vector<ResolvedPackage>, diag::Reported> child =
        resolve_into(canonical_dir.join(dep.path), sources, arena, bag,
                     visited);
    if (child.is_err()) {
      return base::make_err(diag::Reported{});
    }
    for (ResolvedPackage& package : std::move(child).unwrap()) {
      resolved.push_back(std::move(package));
    }
  }

  visited.pop_back();
  return base::make_ok(std::move(resolved));
}

}  // namespace

base::Result<std::vector<ResolvedPackage>, diag::Reported> resolve_package(
    std::string_view dir,
    source::SourceManager& sources,
    mem::Arena& arena,
    diag::DiagBag& bag) {
  base::Result<path::Path, path::PathError> canonical =
      path::Path::from_native(dir);
  if (canonical.is_err()) {
    const u32 index = bag.emit<i18n::Key::PkgInvalidPackageDirectory>(
        diag::Severity::Error, diag::Stage::Pkg, DiagCode::ResolveIoError, dir);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  std::vector<path::Path> visited;
  return resolve_into(std::move(canonical).unwrap(), sources, arena, bag,
                      visited);
}

}  // namespace pkg
