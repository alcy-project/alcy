// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "pkg/resolve.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

namespace {

// Diagnostic codes 2100-2199 are reserved for dependency resolution.
constexpr u32 kResolveIoError = 2100;
constexpr u32 kResolveCycleError = 2101;

std::string join_path(std::string_view dir, std::string_view name) {
  std::string out(dir);
  if (!out.empty() && out.back() != '/') {
    out.push_back('/');
  }
  out.append(name);
  return out;
}

// Lexically normalizes a path: collapses duplicate separators, resolves "."
// and resolvable ".." segments, strips trailing slashes. Only '/' separates
// (valid on Windows APIs too). Symlinks are not resolved, so cycle detection
// covers spelling variants. Empty results normalize to ".".
std::string normalize_path(std::string_view path) {
  const bool absolute = !path.empty() && path.front() == '/';
  std::string out;
  std::vector<usize> starts;
  usize i = absolute ? 1 : 0;
  while (i <= path.size()) {
    usize end = i;
    while (end < path.size() && path[end] != '/') {
      ++end;
    }
    const std::string_view part = path.substr(i, end - i);
    if (part.empty() || part == ".") {
      // Skip.
    } else if (part == "..") {
      if (!starts.empty()) {
        out.resize(starts.back());
        starts.pop_back();
      } else if (!absolute) {
        if (!out.empty()) {
          out.push_back('/');
        }
        out.append("..");
      }
    } else {
      starts.push_back(static_cast<usize>(out.size()));
      if (!out.empty()) {
        out.push_back('/');
      }
      out.append(part);
    }
    i = end + 1;
  }
  if (absolute) {
    return "/" + out;
  }
  return out.empty() ? "." : out;
}

diag::Fallible<std::vector<ResolvedPackage>> resolve_into(
    const std::string& canonical_dir,
    source::SourceManager& sources,
    mem::Arena& arena,
    diag::DiagBag& bag,
    std::vector<std::string>& visited) {
  for (const std::string& seen : visited) {
    if (seen == canonical_dir) {
      const u32 index =
          bag.emit(diag::Severity::Error, kResolveCycleError,
                   "dependency cycle detected at '{}'", canonical_dir);
      (void)index;
      return base::make_err(diag::Fatal{});
    }
  }
  visited.push_back(canonical_dir);

  const std::string manifest_path = join_path(canonical_dir, kManifestFileName);
  base::Result<source::FileId, source::SourceError> loaded =
      sources.load(manifest_path);
  if (loaded.is_err()) {
    const u32 index = bag.emit(diag::Severity::Error, kResolveIoError,
                               "cannot read manifest '{}'", manifest_path);
    (void)index;
    return base::make_err(diag::Fatal{});
  }
  const source::FileId file = std::move(loaded).unwrap();

  diag::Fallible<PackageManifest> parsed =
      parse_manifest(sources.bytes(file), manifest_path, file, bag, arena);
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
    const std::string joined = join_path(canonical_dir, dep.path);
    const std::string canonical = normalize_path(joined);
    diag::Fallible<std::vector<ResolvedPackage>> child =
        resolve_into(canonical, sources, arena, bag, visited);
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
  const std::string canonical = normalize_path(dir);
  std::vector<std::string> visited;
  return resolve_into(canonical, sources, arena, bag, visited);
}

}  // namespace pkg
