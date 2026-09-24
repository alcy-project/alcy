// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/new.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "config/build_config.h"
#include "diag/diagnostic.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/file_handle.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pipeline {

namespace {

// TODO: Move this to fpag (maybe io module?)
bool make_dirs(std::string_view path) {
  std::string current;
  for (usize i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == path::kDefaultPathSeparator) {
      if (!current.empty()) {
#if BUILD_FLAG(IS_OS_WIN)
        ::_mkdir(current.c_str());
#else
        ::mkdir(current.c_str(), 0755);
#endif
      }
    }
    if (i < path.size()) {
      current.push_back(path[i]);
    }
  }
  return true;
}

bool write_text_file(std::string_view path, std::string_view content) {
  // Copy first: fopen requires a null-terminated path, which only an owned
  // copy guarantees.
  const std::string owned(path);
  std::FILE* file = std::fopen(owned.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const usize written = std::fwrite(content.data(), 1, content.size(), file);
  std::fclose(file);
  return written == content.size();
}

}  // namespace

bool valid_package_name(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// Writes alcy.toml and main.al for a validated package directory.
// Refuses to overwrite existing package files.
NewResult write_package_files(PipelineContext& ctx,
                              const path::Path& package_dir,
                              std::string_view name) {
  const path::Path manifest_path = package_dir.join(pkg::kManifestFileName);
  const path::Path main_path = package_dir.join("main.al");
  for (const path::Path& path : {manifest_path, main_path}) {
    io::FileHandle probe;
    if (probe.open(path.c_str(), io::FileAccess::Read)) {
      const u32 index = ctx.bag.emit(
          diag::Severity::Error, kPipelineIoError,
          "'{}' already exists; refusing to overwrite", path.as_view());
      (void)index;
      return base::make_err(0);
    }
  }

  // TODO: Add `include = ["*"]` syntax support.
  const std::string manifest_template = fmt::format(R"([package]
name = "{}"
version = "0.1.0"

[modules]
include = ["main"]

[[bin]]
name = "{}"
path = "main.al")",
                                                    name, name);
  static constexpr std::string_view kMainText =
      "fn main() {\n  // Write your code here.\n}\n";

  if (!make_dirs(package_dir.as_view())) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot create package directory: '{}'",
                                   package_dir.as_view());
    (void)index;
    return base::make_err(0);
  }
  if (!write_text_file(manifest_path.as_view(), manifest_template) ||
      !write_text_file(main_path.as_view(), kMainText)) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot create package files: '{}'",
                                   package_dir.as_view());
    (void)index;
    return base::make_err(0);
  }
  return base::make_ok();
}

// Last canonical segment, for package naming. Empty for roots and ".".
std::string_view dir_basename(std::string_view dir) {
  if (dir.empty() || dir == "." || dir == "..") {
    return {};
  }
  const usize slash = dir.rfind(path::kDefaultPathSeparator);
  if (slash == std::string_view::npos) {
    return dir;
  }
  return dir.substr(slash + 1);
}

// Basename of the process working directory, for `init` without a
// nameable target. Empty when the directory cannot be read.
std::string current_dir_basename() {
  char buffer[4096];
#if BUILD_FLAG(IS_OS_WIN)
  if (::_getcwd(buffer, sizeof(buffer)) == nullptr) {
    return {};
  }
#else
  if (::getcwd(buffer, sizeof(buffer)) == nullptr) {
    return {};
  }
#endif
  const std::string_view path(buffer);
  const usize slash = path.find_last_of("/\\");
  if (slash == std::string_view::npos) {
    return std::string(path);
  }
  return std::string(path.substr(slash + 1));
}

NewResult create_new_package(PipelineContext& ctx,
                             std::string_view target_dir) {
  if (!valid_package_name(target_dir)) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, kPipelineIoError,
        "invalid package name '{}'; use [A-Za-z0-9_-] only", target_dir);
    (void)index;
    return base::make_err(0);
  }

  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(target_dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot create package '{}'", target_dir);
    (void)index;
    return base::make_err(0);
  }
  const path::Path package_dir = std::move(root).unwrap();
  return write_package_files(ctx, package_dir, target_dir);
}

NewResult init_package(PipelineContext& ctx, std::string_view target_dir) {
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(target_dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot init package '{}'", target_dir);
    (void)index;
    return base::make_err(0);
  }
  const path::Path package_dir = std::move(root).unwrap();
  std::string_view name = dir_basename(package_dir.as_view());
  std::string fallback;
  if (name.empty()) {
    fallback = current_dir_basename();
    name = fallback;
  }
  if (!valid_package_name(name)) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, kPipelineIoError,
        "cannot derive a package name from '{}'; use [A-Za-z0-9_-] only",
        target_dir);
    (void)index;
    return base::make_err(0);
  }
  return write_package_files(ctx, package_dir, name);
}

}  // namespace pipeline

