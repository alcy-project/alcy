// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "pipeline/pipeline.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cfg/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/file_handle.h"
#include "path/path.h"
#include "pkg/manifest.h"
#include "pkg/resolve.h"
#include "source/source.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <fileapi.h>
#include <handleapi.h>
#include <minwinbase.h>
#include <winnt.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace pipeline {

namespace {

// Diagnostic codes 2200-2299 are reserved for the pipeline.
constexpr u32 kPipelineIoError = 2200;

bool has_source_extension(std::string_view path) {
  if (path.size() < kSourceExtension.size()) {
    return false;
  }
  return path.substr(path.size() - kSourceExtension.size()) == kSourceExtension;
}

// A directory containing alcy.toml is a nested package: its sources belong
// to that package, so the subtree is skipped. The walk root itself is never
// tested, only its children.
bool is_nested_package(const path::Path& dir) {
  io::FileHandle probe;
  return probe.open(dir.join(pkg::kManifestFileName).as_view(),
                    io::FileAccess::Read);
}

// Collects *.al files under dir, recursively. Directory symlinks are never
// followed, so link cycles are impossible. Unreadable nested entries are
// skipped. Returns false only when the root itself cannot be opened.
bool walk_sources(const path::Path& dir, std::vector<path::Path>& paths) {
#if BUILD_FLAG(IS_OS_WIN)
  WIN32_FIND_DATAA found;
  const std::string pattern =
      std::string(dir.as_view()) + path::kDefaultPathSeparator + "*";
  HANDLE handle = ::FindFirstFileA(pattern.c_str(), &found);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  do {
    const std::string_view name(found.cFileName);
    if (name == "." || name == "..") {
      continue;
    }
    const path::Path full = dir.join(name);
    const bool is_dir =
        (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool is_link =
        (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (is_dir && !is_link) {
      if (!is_nested_package(full)) {
        walk_sources(full, paths);
      }
    } else if (!is_dir && has_source_extension(full.as_view())) {
      paths.push_back(full);
    }
  } while (::FindNextFileA(handle, &found) != 0);
  ::FindClose(handle);
  return true;
#else
  DIR* handle = ::opendir(dir.c_str());
  if (handle == nullptr) {
    return false;
  }
  while (dirent* entry = ::readdir(handle)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    const path::Path full = dir.join(name);
    struct stat info;
    // lstat: never follow symlinks, so link cycles are impossible.
    if (::lstat(full.c_str(), &info) != 0) {
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      if (!is_nested_package(full)) {
        walk_sources(full, paths);
      }
    } else if (S_ISREG(info.st_mode) && has_source_extension(full.as_view())) {
      paths.push_back(full);
    }
  }
  ::closedir(handle);
  return true;
#endif
}

}  // namespace

diag::SourceText fetch_source(source::FileId id, const void* ctx) {
  const auto* sources = static_cast<const source::SourceManager*>(ctx);
  if (id >= sources->file_count()) {
    return {};
  }
  return {sources->name(id), sources->bytes(id)};
}

diag::Fallible<DiscoveredSources> discover_sources(
    std::string_view dir,
    source::SourceManager& sources,
    diag::DiagBag& bag) {
  base::Result<path::Path, path::PathError> root = path::Path::from_native(dir);
  if (root.is_err()) {
    const u32 index = bag.emit(diag::Severity::Error, kPipelineIoError,
                               "invalid source directory '{}'", dir);
    (void)index;
    return base::make_err(diag::Fatal{});
  }
  const path::Path root_path = std::move(root).unwrap();
  std::vector<path::Path> paths;
  if (!walk_sources(root_path, paths)) {
    const u32 index = bag.emit(diag::Severity::Error, kPipelineIoError,
                               "source directory '{}' is not accessible", dir);
    (void)index;
    return base::make_err(diag::Fatal{});
  }
  if (paths.empty()) {
    const u32 index = bag.emit(diag::Severity::Error, kPipelineIoError,
                               "no source files found under '{}'", dir);
    (void)index;
    return base::make_err(diag::Fatal{});
  }
  std::sort(paths.begin(), paths.end());

  DiscoveredSources discovered;
  discovered.root = std::string(root_path.as_view());
  for (const path::Path& path : paths) {
    base::Result<source::FileId, source::SourceError> loaded =
        sources.load(path.as_view());
    if (loaded.is_err()) {
      const u32 index =
          bag.emit(diag::Severity::Error, kPipelineIoError,
                   "cannot read source file '{}'", path.as_view());
      (void)index;
      return base::make_err(diag::Fatal{});
    }
    discovered.files.push_back(std::move(loaded).unwrap());
  }
  return base::make_ok(std::move(discovered));
}

diag::Fallible<ProjectBuild> compile_project(
    std::span<const pkg::ResolvedPackage> packages,
    source::SourceManager& sources,
    diag::DiagBag& bag) {
  ProjectBuild build;
  for (const pkg::ResolvedPackage& package : packages) {
    diag::Fallible<DiscoveredSources> discovered =
        discover_sources(package.dir.as_view(), sources, bag);
    if (discovered.is_err()) {
      return base::make_err(diag::Fatal{});
    }
    build.files_loaded +=
        static_cast<u32>(std::move(discovered).unwrap().files.size());
    ++build.packages;
  }
  return base::make_ok(build);
}

}  // namespace pipeline
