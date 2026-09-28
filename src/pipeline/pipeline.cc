// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/pipeline.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/file_handle.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "pkg/resolve.h"
#include "source/source.h"

#if BUILD_FLAG(IS_OS_WIN)
// windows.h first, not the handful of SDK headers this file happens to
// use: a bare <fileapi.h> reaches winnt.h before anything has defined
// _AMD64, and winnt.h rejects that. It used to work because another header
// in the chain pulled windows.h in first, so this compiled by accident until
// fpag stopped doing that on purpose.
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace pipeline {

namespace {

bool has_source_extension(std::string_view path) {
  if (path.size() < path::SOURCE_EXTENSION.size()) {
    return false;
  }
  return path.substr(path.size() - path::SOURCE_EXTENSION.size()) ==
         path::SOURCE_EXTENSION;
}

// A directory containing alcy.toml is a nested package: its sources belong
// to that package, so the subtree is skipped. The walk root itself is never
// tested, only its children.
bool is_nested_package(const path::Path& dir) {
  io::FileHandle probe;
  return probe.open(dir.join(pkg::MANIFEST_FILE_NAME).as_view(),
                    io::FileAccess::Read);
}

// Collects *.al files under dir, recursively. Symlinked files are
// followed, symlinked directories are not, so link cycles are
// impossible. Unreadable nested entries are skipped. Returns false only
// when the root itself cannot be opened.
bool walk_sources(const path::Path& dir, std::vector<path::Path>& paths) {
#if BUILD_FLAG(IS_OS_WIN)
  WIN32_FIND_DATAA found;
  const std::string pattern =
      std::string(dir.as_view()) + path::DEFAULT_PATH_SEPARATOR + "*";
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
    if (::lstat(full.c_str(), &info) != 0) {
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      if (!is_nested_package(full)) {
        walk_sources(full, paths);
      }
      continue;
    }
    // A regular file is a source when it carries the extension. A
    // symlinked file is resolved and treated the same way; a symlinked
    // *directory* resolves to a directory and so is left alone, which
    // keeps a link cycle from making the walk unbounded.
    bool is_source = S_ISREG(info.st_mode);
    if (!is_source && S_ISLNK(info.st_mode) &&
        ::stat(full.c_str(), &info) == 0) {
      is_source = S_ISREG(info.st_mode);
    }
    if (is_source && has_source_extension(full.as_view())) {
      paths.push_back(full);
    }
  }
  ::closedir(handle);
  return true;
#endif
}

}  // namespace

base::Result<DiscoveredSources, diag::Reported> discover_sources(
    std::string_view dir,
    source::SourceManager& sources,
    diag::DiagBag& bag) {
  base::Result<path::Path, path::PathError> root = path::Path::from_native(dir);
  if (root.is_err()) {
    const u32 index = bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                               "invalid source directory '{}'", dir);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const path::Path root_path = std::move(root).unwrap();
  std::vector<path::Path> paths;
  if (!walk_sources(root_path, paths)) {
    const u32 index = bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                               "source directory '{}' is not accessible", dir);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (paths.empty()) {
    const u32 index = bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                               "no source files found under '{}'", dir);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  std::sort(paths.begin(), paths.end());

  DiscoveredSources discovered;
  discovered.root = std::string(root_path.as_view());
  for (const path::Path& path : paths) {
    base::Result<source::FileId, source::SourceError> loaded =
        sources.load(path.as_view());
    if (loaded.is_err()) {
      const u32 index =
          bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                   "cannot read source file '{}'", path.as_view());
      (void)index;
      return base::make_err(diag::Reported{});
    }
    discovered.files.push_back(std::move(loaded).unwrap());
  }
  return base::make_ok(std::move(discovered));
}

base::Result<ProjectBuild, diag::Reported> compile_project(
    std::span<const pkg::ResolvedPackage> packages,
    source::SourceManager& sources,
    diag::DiagBag& bag) {
  ProjectBuild build;
  for (const pkg::ResolvedPackage& package : packages) {
    base::Result<DiscoveredSources, diag::Reported> discovered =
        discover_sources(package.dir.as_view(), sources, bag);
    if (discovered.is_err()) {
      return base::make_err(diag::Reported{});
    }
    const DiscoveredSources found = std::move(discovered).unwrap();
    build.files_loaded += static_cast<u32>(found.files.size());
    ++build.packages;
  }
  return base::make_ok(build);
}

}  // namespace pipeline
