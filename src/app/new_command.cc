// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/new_command.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "app/driver_context.h"
#include "app/result_code.h"
#include "base/logger.h"
#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pkg/manifest.h"

#if BUILD_FLAG(IS_OS_WIN)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace app {

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

ResultCode run_new(std::string_view target_dir) {
  if (!valid_package_name(target_dir)) {
    base::logger.wo_prefix("invalid package name '{}'; use [A-Za-z0-9_-] only",
                           target_dir);
    return ResultCode::ArgParseError;
  }

  DriverContext ctx;
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(target_dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "cannot create package '{}'", target_dir);
    (void)index;
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  const path::Path package_dir = std::move(root).unwrap();
  const path::Path manifest_path = package_dir.join(pkg::kManifestFileName);
  const path::Path main_path = package_dir.join("main.al");

  // TODO: Add `include = ["*"]` syntax support.
  const std::string manifest_template =
      fmt::format(R"([package]
name = "{}"
version = "0.1.0"

[modules]
include = ["main"]

[[bin]]
name = "{}"
path = "main.al")",
                  package_dir.as_view(), package_dir.as_view());
  static constexpr std::string_view kMainText =
      "fn main() {\n  // Write your code here.\n}\n";

  if (!make_dirs(package_dir.as_view()) ||
      !write_text_file(manifest_path.as_view(), manifest_template) ||
      !write_text_file(main_path.as_view(), kMainText)) {
    const u32 index =
        ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                     "cannot create package '{}'", package_dir.as_view());
    (void)index;
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  base::logger.wo_prefix("created package '{}'", package_dir.as_view());
  return ResultCode::Success;
}

}  // namespace app
