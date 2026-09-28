// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/new.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/build_config.h"
#include "diag/diagnostic.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/file_handle.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/vcs.h"
#include "pkg/manifest.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pipeline {

namespace {

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
                              std::string_view name,
                              Vcs vcs) {
  const path::Path manifest_path = package_dir.join(pkg::MANIFEST_FILE_NAME);
  const path::Path main_path = package_dir.join("main.al");
  // Only the chosen VCS contributes a file, so `none` neither writes nor
  // refuses to overwrite an ignore file the user may have put there.
  std::vector<path::Path> files{manifest_path, main_path};
  if (vcs == Vcs::Git) {
    files.push_back(package_dir.join(".gitignore"));
  }
  for (const path::Path& path : files) {
    io::FileHandle probe;
    if (probe.open(path.c_str(), io::FileAccess::Read)) {
      const u32 index = ctx.bag.emit(
          diag::Severity::Error, PIPELINE_IO_ERROR,
          "'{}' already exists; refusing to overwrite", path.as_view());
      (void)index;
      return base::make_err(0);
    }
  }

  const std::string manifest_template = fmt::format(R"([package]
name = "{}"
version = "0.1.0"

[modules]
include = ["main"]

[dependencies]
"alcy/std/*" = {{}}

[[bin]]
name = "{}"
path = "main.al")",
                                                    name, name);
  static constexpr std::string_view MAIN_TEXT =
      "fn main() {\n  println(\"hello world\")\n}\n";
  static constexpr std::string_view GIT_IGNORE_TEXT = "/out/\n";

  if (ensure_directories(ctx, package_dir.as_view()).is_err()) {
    return base::make_err(0);
  }
  if (!write_text_file(manifest_path.as_view(), manifest_template) ||
      !write_text_file(main_path.as_view(), MAIN_TEXT) ||
      (vcs == Vcs::Git &&
       !write_text_file(package_dir.join(".gitignore").as_view(),
                        GIT_IGNORE_TEXT))) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
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
  const usize slash = dir.rfind(path::DEFAULT_PATH_SEPARATOR);
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
                             std::string_view target_dir,
                             Vcs vcs) {
  if (!valid_package_name(target_dir)) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, PIPELINE_IO_ERROR,
        "invalid package name '{}'; use [A-Za-z0-9_-] only", target_dir);
    (void)index;
    return base::make_err(0);
  }

  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(target_dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot create package '{}'", target_dir);
    (void)index;
    return base::make_err(0);
  }
  const path::Path package_dir = std::move(root).unwrap();
  return write_package_files(ctx, package_dir, target_dir, vcs);
}

NewResult init_package(PipelineContext& ctx,
                       std::string_view target_dir,
                       Vcs vcs) {
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(target_dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
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
        diag::Severity::Error, PIPELINE_IO_ERROR,
        "cannot derive a package name from '{}'; use [A-Za-z0-9_-] only",
        target_dir);
    (void)index;
    return base::make_err(0);
  }
  return write_package_files(ctx, package_dir, name, vcs);
}

}  // namespace pipeline

