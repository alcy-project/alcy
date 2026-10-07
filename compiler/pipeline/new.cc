// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/new.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/build_config.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/file_handle.h"
#include "fpag/io/io_util.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
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

// Reads a text file whole; empty means it could not be read.
std::string read_text_file(std::string_view path) {
  return io::read_file(std::string(path));
}

bool file_exists(std::string_view path) {
  io::FileHandle probe;
  return probe.open(path, io::FileAccess::Read);
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

// The process's own directory, absolute. Empty when it cannot be read.
std::string current_dir_path() {
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
  return std::string(buffer);
}

// The suite a new package would join: its root, its manifest, and the
// suite-relative path the package enters under.
struct EnclosingSuite {
  path::Path root;
  pkg::SuiteManifest manifest;
  std::string member;
};

// `owner/name`, or `name` when the suite declares no owner.
std::string suite_spelling(const pkg::SuiteManifest& suite) {
  if (suite.owner.empty()) {
    return std::string(suite.name);
  }
  return fmt::format("{}/{}", suite.owner, suite.name);
}

// Walks from the package directory's parent to the filesystem root,
// looking for the nearest manifest. A suite manifest claims the
// package as a member; a package manifest ends the walk, because a
// package is not a suite. Diagnostics go to the bag; no value means
// the package is standalone, or that the walk reported an error.
std::optional<EnclosingSuite> find_enclosing_suite(
    PipelineContext& ctx,
    const path::Path& package_dir) {
  path::Path at = package_dir.parent();
  std::string member(dir_basename(package_dir.as_view()));
  while (true) {
    const path::Path manifest_path = at.join(pkg::MANIFEST_FILE_NAME);
    if (file_exists(manifest_path.as_view())) {
      const std::string bytes = read_text_file(manifest_path.as_view());
      switch (pkg::probe_manifest_kind(bytes)) {
        case pkg::ManifestKind::Suite: {
          base::Result<pkg::SuiteManifest, diag::Reported> parsed =
              pkg::parse_suite_manifest(bytes, manifest_path.as_view(),
                                        source::UNKNOWN_FILE, ctx.bag,
                                        ctx.arena);
          if (parsed.is_err()) {
            return std::nullopt;
          }
          pkg::SuiteManifest suite = std::move(parsed).unwrap();
          base::Result<void, pkg::SuiteError> verified =
              pkg::verify_suite_manifest(suite);
          if (verified.is_err()) {
            pkg::report_suite_error(std::move(verified).unwrap_err(),
                                    manifest_path.as_view(), ctx.bag);
            return std::nullopt;
          }
          return EnclosingSuite{std::move(at), suite, std::move(member)};
        }
        case pkg::ManifestKind::Package: return std::nullopt;
        case pkg::ManifestKind::Unknown:
          // Report through the suite parser: the nearest manifest is
          // the one the scaffold would have joined.
          (void)pkg::parse_suite_manifest(bytes, manifest_path.as_view(),
                                          source::UNKNOWN_FILE, ctx.bag,
                                          ctx.arena);
          return std::nullopt;
      }
    }
    const path::Path parent = at.parent();
    if (parent == at) {
      return std::nullopt;
    }
    member = std::string(dir_basename(at.as_view())) + "/" + member;
    at = parent;
  }
}

// Inserts `member` into the `packages` array of a suite manifest,
// preserving everything else. Strings and comments are skipped while
// the array's end is found, so brackets in either do not end it.
// False when the array cannot be found.
bool insert_suite_member(std::string_view text,
                         std::string_view member,
                         std::string* out) {
  constexpr std::string_view KEY = "packages";
  const usize suite = text.find("[suite]");
  const usize key = text.find(KEY);
  if (suite == std::string_view::npos || key == std::string_view::npos ||
      key < suite) {
    return false;
  }
  usize at = key + KEY.size();
  while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
    ++at;
  }
  if (at >= text.size() || text[at] != '=') {
    return false;
  }
  ++at;
  while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
    ++at;
  }
  if (at >= text.size() || text[at] != '[') {
    return false;
  }
  const usize open = at;
  usize close = open + 1;
  i32 depth = 1;
  bool in_string = false;
  bool in_comment = false;
  char quote = 0;
  for (; close < text.size(); ++close) {
    const char c = text[close];
    if (in_comment) {
      if (c == '\n') {
        in_comment = false;
      }
      continue;
    }
    if (in_string) {
      if (c == quote) {
        in_string = false;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      in_string = true;
      quote = c;
      continue;
    }
    if (c == '#') {
      in_comment = true;
      continue;
    }
    if (c == '[') {
      ++depth;
    } else if (c == ']') {
      --depth;
      if (depth == 0) {
        break;
      }
    }
  }
  if (close >= text.size()) {
    return false;
  }

  const std::string_view inner = text.substr(open + 1, close - open - 1);
  const std::string quoted = "\"" + std::string(member) + "\"";
  std::string edited(text);
  if (inner.find_first_not_of(" \t\r\n") == std::string_view::npos) {
    edited.insert(open + 1, quoted);
  } else {
    usize last = close;
    while (last > open + 1) {
      const char c = text[last - 1];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        --last;
        continue;
      }
      break;
    }
    if (text[last - 1] == ',') {
      if (inner.find('\n') != std::string_view::npos) {
        const usize line = text.rfind('\n', last - 1);
        const usize line_start = line == std::string_view::npos ? 0 : line + 1;
        usize indent = line_start;
        while (indent < text.size() &&
               (text[indent] == ' ' || text[indent] == '\t')) {
          ++indent;
        }
        edited.insert(
            last,
            "\n" + std::string(text.substr(line_start, indent - line_start)) +
                quoted + ",");
      } else {
        edited.insert(last, " " + quoted + ",");
      }
    } else {
      edited.insert(last, ", " + quoted);
    }
  }
  *out = std::move(edited);
  return true;
}

// Whether the suite may take the member: no listed path equals it,
// and no other member is addressed by its name (ADR-0057). Checked
// before any file is written, so a refusal leaves nothing behind.
bool suite_accepts_member(PipelineContext& ctx,
                          const EnclosingSuite& suite,
                          std::string_view member) {
  const std::string spelling = suite_spelling(suite.manifest);
  for (u32 i = 0; i < suite.manifest.package_count; ++i) {
    const std::string_view listed = suite.manifest.packages[i];
    if (listed == member) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineSuiteAlreadyHasMember>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
          spelling, member);
      (void)index;
      return false;
    }
    if (pkg::suite_member_name(listed) == pkg::suite_member_name(member)) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineSuiteHasMemberNamed>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
          spelling, pkg::suite_member_name(member));
      (void)index;
      return false;
    }
  }
  return true;
}

// Adds the member to the suite manifest.
bool append_member(PipelineContext& ctx,
                   const EnclosingSuite& suite,
                   std::string_view member) {
  const path::Path manifest_path = suite.root.join(pkg::MANIFEST_FILE_NAME);
  const std::string bytes = read_text_file(manifest_path.as_view());
  std::string edited;
  if (bytes.empty() || !insert_suite_member(bytes, member, &edited) ||
      !write_text_file(manifest_path.as_view(), edited)) {
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineCannotUpdateSuiteManifest>(
            diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
            member, manifest_path.as_view());
    (void)index;
    return false;
  }
  return true;
}

// Writes alcy.toml and main.al for a validated package directory, and
// refuses to overwrite existing package files. A member writes no
// ignore file: the suite root's build output is the suite's, and its
// `.gitignore` names it.
bool write_package_files(PipelineContext& ctx,
                         const path::Path& package_dir,
                         std::string_view name,
                         Vcs vcs,
                         bool in_suite) {
  const path::Path manifest_path = package_dir.join(pkg::MANIFEST_FILE_NAME);
  const path::Path main_path = package_dir.join("main.al");
  const path::Path ignore_path = package_dir.join(".gitignore");
  const bool writes_ignore = vcs == Vcs::Git && !in_suite;
  std::vector<path::Path> files{manifest_path, main_path};
  if (writes_ignore) {
    files.push_back(ignore_path);
  }
  for (const path::Path& path : files) {
    io::FileHandle probe;
    if (probe.open(path.c_str(), io::FileAccess::Read)) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelinePathExists>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
          path.as_view());
      (void)index;
      return false;
    }
  }

  const std::string manifest_template = in_suite ? fmt::format(R"([package]
name = "{}"
version.suite = true
license.suite = true

[modules]
include = ["main"]

[dependencies]
"alcy/std/*" = {{}}

[[bin]]
name = "{}"
path = "main.al")",
                                                               name, name)
                                                 : fmt::format(R"([package]
name = "{}"
version = "0.1.0"
license = ""

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
    return false;
  }
  if (!write_text_file(manifest_path.as_view(), manifest_template) ||
      !write_text_file(main_path.as_view(), MAIN_TEXT) ||
      (writes_ignore &&
       !write_text_file(ignore_path.as_view(), GIT_IGNORE_TEXT))) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotCreatePackageFiles>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        package_dir.as_view());
    (void)index;
    return false;
  }
  return true;
}

// Writes alcy.toml and .gitignore for a suite. The suite owns the
// build output its members share, so `/out/` is ignored here.
bool write_suite_files(PipelineContext& ctx,
                       const path::Path& suite_dir,
                       std::string_view owner,
                       std::string_view name,
                       Vcs vcs) {
  const path::Path manifest_path = suite_dir.join(pkg::MANIFEST_FILE_NAME);
  const path::Path ignore_path = suite_dir.join(".gitignore");
  const bool writes_ignore = vcs == Vcs::Git;
  std::vector<path::Path> files{manifest_path};
  if (writes_ignore) {
    files.push_back(ignore_path);
  }
  for (const path::Path& path : files) {
    io::FileHandle probe;
    if (probe.open(path.c_str(), io::FileAccess::Read)) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelinePathExists>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
          path.as_view());
      (void)index;
      return false;
    }
  }

  const std::string manifest = fmt::format(R"([suite]
name = "{}"
owner = "{}"
version = "0.1.0"
license = ""
packages = []
)",
                                           name, owner);
  static constexpr std::string_view GIT_IGNORE_TEXT = "/out/\n";

  if (ensure_directories(ctx, suite_dir.as_view()).is_err()) {
    return false;
  }
  if (!write_text_file(manifest_path.as_view(), manifest) ||
      (writes_ignore &&
       !write_text_file(ignore_path.as_view(), GIT_IGNORE_TEXT))) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotCreateSuiteFiles>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        suite_dir.as_view());
    (void)index;
    return false;
  }
  return true;
}

// Splits a `--suite` value into owner and name; either `<name>` or
// `<owner>/<name>`, each segment a package name.
bool parse_suite_spec(PipelineContext& ctx,
                      std::string_view spec,
                      std::string* owner,
                      std::string* name) {
  usize slash = spec.find('/');
  std::string_view owner_part;
  std::string_view name_part = spec;
  if (slash != std::string_view::npos) {
    owner_part = spec.substr(0, slash);
    name_part = spec.substr(slash + 1);
    if (owner_part.empty() || name_part.find('/') != std::string_view::npos) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineInvalidSuiteName>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
          spec);
      (void)index;
      return false;
    }
  }
  if (!valid_package_name(name_part) ||
      (!owner_part.empty() && !valid_package_name(owner_part))) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineInvalidSuiteName>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError, spec);
    (void)index;
    return false;
  }
  *owner = std::string(owner_part);
  *name = std::string(name_part);
  return true;
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

NewResult create_new_package(PipelineContext& ctx,
                             std::string_view target_dir,
                             Vcs vcs) {
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(target_dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotCreatePackage>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        target_dir);
    (void)index;
    return base::make_err(0);
  }
  const path::Path package_dir = std::move(root).unwrap();
  const std::string_view name = dir_basename(package_dir.as_view());
  if (!valid_package_name(name)) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineInvalidPackageName>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        target_dir);
    (void)index;
    return base::make_err(0);
  }

  const std::optional<EnclosingSuite> suite =
      find_enclosing_suite(ctx, package_dir);
  if (!suite.has_value() && ctx.bag.has_errors()) {
    return base::make_err(0);
  }
  std::string spelling;
  if (suite.has_value()) {
    spelling = suite_spelling(suite->manifest);
    if (!suite_accepts_member(ctx, *suite, suite->member)) {
      return base::make_err(0);
    }
  }
  if (!write_package_files(ctx, package_dir, name, vcs, suite.has_value())) {
    return base::make_err(0);
  }
  if (suite.has_value() && !append_member(ctx, *suite, suite->member)) {
    return base::make_err(0);
  }
  return base::make_ok(
      ScaffoldResult{.name = std::string(name), .suite = std::move(spelling)});
}

NewResult init_package(PipelineContext& ctx,
                       std::string_view target_dir,
                       Vcs vcs) {
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(target_dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotInitPackage>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        target_dir);
    (void)index;
    return base::make_err(0);
  }
  path::Path package_dir = std::move(root).unwrap();
  std::string_view name = dir_basename(package_dir.as_view());
  if (name.empty()) {
    // "." has no basename; the process's own directory names the
    // package and anchors the walk to the suite above it.
    const std::string here = current_dir_path();
    base::Result<path::Path, path::PathError> cwd =
        path::Path::from_native(here);
    if (cwd.is_ok()) {
      package_dir = std::move(cwd).unwrap();
      name = dir_basename(package_dir.as_view());
    }
  }
  if (!valid_package_name(name)) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotDerivePackageName>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        target_dir);
    (void)index;
    return base::make_err(0);
  }

  const std::optional<EnclosingSuite> suite =
      find_enclosing_suite(ctx, package_dir);
  if (!suite.has_value() && ctx.bag.has_errors()) {
    return base::make_err(0);
  }
  std::string spelling;
  if (suite.has_value()) {
    spelling = suite_spelling(suite->manifest);
    if (!suite_accepts_member(ctx, *suite, suite->member)) {
      return base::make_err(0);
    }
  }
  if (!write_package_files(ctx, package_dir, name, vcs, suite.has_value())) {
    return base::make_err(0);
  }
  if (suite.has_value() && !append_member(ctx, *suite, suite->member)) {
    return base::make_err(0);
  }
  return base::make_ok(
      ScaffoldResult{.name = std::string(name), .suite = std::move(spelling)});
}

NewResult create_new_suite(PipelineContext& ctx,
                           std::string_view target_dir,
                           std::string_view spec,
                           Vcs vcs) {
  std::string owner;
  std::string name;
  if (!parse_suite_spec(ctx, spec, &owner, &name)) {
    return base::make_err(0);
  }
  // `new --suite tools` makes ./tools; a named target overrides that.
  const std::string_view dir = (target_dir.empty() || target_dir == ".")
                                   ? std::string_view(name)
                                   : target_dir;
  base::Result<path::Path, path::PathError> root = path::Path::from_native(dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotCreateSuite>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        target_dir);
    (void)index;
    return base::make_err(0);
  }
  if (!write_suite_files(ctx, std::move(root).unwrap(), owner, name, vcs)) {
    return base::make_err(0);
  }
  return base::make_ok(ScaffoldResult{.name = std::move(name), .suite = {}});
}

NewResult init_suite(PipelineContext& ctx,
                     std::string_view target_dir,
                     std::string_view spec,
                     Vcs vcs) {
  std::string owner;
  std::string name;
  if (!parse_suite_spec(ctx, spec, &owner, &name)) {
    return base::make_err(0);
  }
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(target_dir);
  if (root.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotCreateSuite>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        target_dir);
    (void)index;
    return base::make_err(0);
  }
  if (!write_suite_files(ctx, std::move(root).unwrap(), owner, name, vcs)) {
    return base::make_err(0);
  }
  return base::make_ok(ScaffoldResult{.name = std::move(name), .suite = {}});
}

}  // namespace pipeline
