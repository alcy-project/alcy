// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/manifest.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/messages.h"
#include "pkg/arena_copy.h"
#include "source/source.h"

// clang-format off
// Umbrella header provides the .inl implementations; keep it whole.
#include "toml++/toml.hpp"  // IWYU pragma: keep
// Other headers must be included after toml.hpp
#include "toml++/impl/array.hpp"
#include "toml++/impl/node.hpp"
#include "toml++/impl/parse_error.hpp"
#include "toml++/impl/parse_result.hpp"
#include "toml++/impl/parser.hpp"
#include "toml++/impl/source_region.hpp"
#include "toml++/impl/table.hpp"
#include "pkg/diag_code.h"
// clang-format on

namespace pkg {

namespace {

// Converts a 1-based toml line/column into a byte offset, clamped.
// toml++ counts columns in code points, so the column is walked over
// the line rather than added as a byte count; otherwise any non-ASCII
// character before the error shifts the reported position right.
u32 line_col_to_offset(std::string_view bytes, u32 line, u32 column) {
  u32 offset = 0;
  for (u32 current = 1; current < line && offset < bytes.size(); ++offset) {
    if (bytes[offset] == '\n') {
      ++current;
    }
  }
  if (column <= 1) {
    return offset;
  }
  u32 line_end = offset;
  while (line_end < bytes.size() && bytes[line_end] != '\n') {
    ++line_end;
  }
  // Step over `column - 1` characters, each beginning at a byte that is
  // not a continuation. Running off the end clamps to the line end, so
  // a span past the last character still points inside the file.
  for (u32 remaining = column - 1; remaining > 0; --remaining) {
    while (offset < line_end &&
           (static_cast<u8>(bytes[offset]) & 0xC0) == 0x80) {
      ++offset;
    }
    if (offset >= line_end) {
      return line_end;
    }
    ++offset;
  }
  return offset;
}

diag::Span toml_span(std::string_view bytes,
                     source::FileId file,
                     const toml::source_region& region) {
  const u32 begin =
      line_col_to_offset(bytes, region.begin.line, region.begin.column);
  const u32 end = line_col_to_offset(bytes, region.end.line, region.end.column);
  const u32 length = end > begin ? end - begin : 0;
  return {.file = file, .offset = begin, .length = length};
}

base::Result<Dependency, diag::Reported> parse_dependency(
    diag::DiagBag& bag,
    std::string_view filename,
    mem::Arena& arena,
    std::string_view spec,
    const toml::node& node);

base::Result<PackageManifest, diag::Reported> semantic_error(
    diag::DiagBag& bag,
    std::string_view filename,
    std::string_view message) {
  // Semantic errors name the manifest in the message and carry no span;
  // only syntax errors have a position to report.
  const u32 index = bag.emit<i18n::Key::PkgManifestInvalid>(
      diag::Severity::Error, diag::Stage::Pkg, DiagCode::ManifestSemanticError,
      filename, message);
  (void)index;
  return base::make_err(diag::Reported{});
}

// Reads an optional string field of a dependency table. Empty and
// non-string values are both errors, since neither can name a source.
base::Result<std::string_view, diag::Reported> dep_string(
    diag::DiagBag& bag,
    std::string_view filename,
    std::string_view dep,
    const toml::table& table,
    std::string_view field,
    bool* present) {
  const auto it = table.find(field);
  if (it == table.end()) {
    *present = false;
    return base::make_ok(std::string_view{});
  }
  *present = true;
  const auto value = it->second.value<std::string_view>();
  if (!value.has_value() || value->empty()) {
    bag.emit<i18n::Key::PkgDependencyEmptyString>(
        diag::Severity::Error, diag::Stage::Pkg,
        DiagCode::ManifestSemanticError, filename, dep, field);
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(*value);
}

// Emits one manifest semantic error and returns it; the return type is
// the caller's, so a field parser can fail directly.
template <typename T>
base::Result<T, diag::Reported> manifest_field_error(diag::DiagBag& bag,
                                                     std::string_view filename,
                                                     std::string_view message) {
  const u32 index = bag.emit<i18n::Key::PkgManifestInvalid>(
      diag::Severity::Error, diag::Stage::Pkg, DiagCode::ManifestSemanticError,
      filename, message);
  (void)index;
  return base::make_err(diag::Reported{});
}

// Whether a node is exactly `{ suite = true }` (the dotted
// `field.suite = true` parses the same), the spelling that takes the
// suite's value (ADR-0057).
bool is_suite_marker(const toml::node& node) {
  const toml::table* const table = node.as_table();
  if (table == nullptr || table->size() != 1) {
    return false;
  }
  const auto suite = table->find("suite");
  if (suite == table->end()) {
    return false;
  }
  const auto value = suite->second.value<bool>();
  return value.has_value() && *value;
}

// A string field that may spell `{ suite = true }` instead. `present`
// tells a declared value from an absent key, which an empty string
// needs.
struct InheritedString {
  std::string_view text;
  bool present = false;
  bool from_suite = false;
};

base::Result<InheritedString, diag::Reported> inherited_string(
    diag::DiagBag& bag,
    std::string_view filename,
    mem::Arena& arena,
    const toml::table& table,
    std::string_view field,
    std::string_view bad_message) {
  InheritedString out;
  const auto it = table.find(field);
  if (it == table.end()) {
    return base::make_ok(out);
  }
  if (const auto value = it->second.value<std::string_view>();
      value.has_value()) {
    out.text = copy_str(arena, *value);
    out.present = true;
    return base::make_ok(out);
  }
  if (is_suite_marker(it->second)) {
    out.present = true;
    out.from_suite = true;
    return base::make_ok(out);
  }
  return manifest_field_error<InheritedString>(bag, filename, bad_message);
}

// The `version` field of a manifest: a string, or `{ suite = true }`
// for a member that takes the suite's.
struct ManifestVersionField {
  Version version;
  bool present = false;
  bool from_suite = false;
};

base::Result<ManifestVersionField, diag::Reported> manifest_version(
    diag::DiagBag& bag,
    std::string_view filename,
    const toml::table& table,
    std::string_view bad_shape_message,
    std::string_view bad_version_message) {
  ManifestVersionField out;
  const auto it = table.find("version");
  if (it == table.end()) {
    return base::make_ok(out);
  }
  if (const auto text = it->second.value<std::string_view>();
      text.has_value()) {
    base::Result<Version, VersionError> parsed = parse_version(*text);
    if (parsed.is_err()) {
      return manifest_field_error<ManifestVersionField>(bag, filename,
                                                        bad_version_message);
    }
    out.version = std::move(parsed).unwrap();
    out.present = true;
    return base::make_ok(out);
  }
  if (is_suite_marker(it->second)) {
    out.from_suite = true;
    return base::make_ok(out);
  }
  return manifest_field_error<ManifestVersionField>(bag, filename,
                                                    bad_shape_message);
}

base::Result<Dependency, diag::Reported> parse_dependency(
    diag::DiagBag& bag,
    std::string_view filename,
    mem::Arena& arena,
    std::string_view spec,
    const toml::node& node) {
  if (!node.is_table()) {
    bag.emit<i18n::Key::PkgDependencyNotATable>(
        diag::Severity::Error, diag::Stage::Pkg,
        DiagCode::ManifestSemanticError, filename, spec);
    return base::make_err(diag::Reported{});
  }
  const toml::table& table = *node.as_table();
  // The specifier is one to three slash-separated segments. One is a
  // local directory aliased by the key; two name a package; three name
  // a package in a suite, or every member of one with a trailing `/*`.
  // A glob anywhere else is not a pattern the resolver speaks.
  std::string_view segments[3];
  u32 parts = 0;
  usize start = 0;
  while (start <= spec.size() && parts < 4) {
    usize slash = spec.find('/', start);
    if (slash == std::string_view::npos) {
      slash = spec.size();
    }
    if (parts < 3) {
      segments[parts] = spec.substr(start, slash - start);
    }
    ++parts;
    start = slash + 1;
  }
  if (parts < 1 || parts > 3) {
    bag.emit<i18n::Key::PkgDependencyBadSpecifier>(
        diag::Severity::Error, diag::Stage::Pkg,
        DiagCode::ManifestSemanticError, filename, spec);
    return base::make_err(diag::Reported{});
  }
  for (u32 i = 0; i < parts; ++i) {
    const bool glob_here = segments[i] == "*";
    if (segments[i].empty() || (glob_here && !(i == 2 && parts == 3))) {
      bag.emit<i18n::Key::PkgDependencyBadSpecifier>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ManifestSemanticError, filename, spec);
      return base::make_err(diag::Reported{});
    }
  }
  if (parts == 1) {
    // The old shape, unchanged: a local directory under an alias.
    bool has_path = false;
    base::Result<std::string_view, diag::Reported> path =
        dep_string(bag, filename, spec, table, "path", &has_path);
    if (path.is_err()) {
      return base::make_err(diag::Reported{});
    }
    if (!has_path) {
      bag.emit<i18n::Key::PkgDependencyNeedsPath>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ManifestSemanticError, filename, spec);
      return base::make_err(diag::Reported{});
    }
    return base::make_ok(Dependency{
        .spec = copy_str(arena, spec),
        .owner = {},
        .suite = {},
        .member = {},
        .suite_glob = false,
        .source = DependencySource::Path,
        .path = copy_str(arena, std::move(path).unwrap()),
        .version = {},
        .version_req = {},
        .git = {},
        .git_ref_kind = {},
        .git_ref = {},
        .registry = {},
        .name = copy_str(arena, spec),
    });
  }
  bool has_path = false;
  bool has_version = false;
  bool has_git = false;
  bool has_branch = false;
  bool has_tag = false;
  bool has_rev = false;
  bool has_registry = false;
  base::Result<std::string_view, diag::Reported> path =
      dep_string(bag, filename, spec, table, "path", &has_path);
  base::Result<std::string_view, diag::Reported> version =
      dep_string(bag, filename, spec, table, "version", &has_version);
  base::Result<std::string_view, diag::Reported> git =
      dep_string(bag, filename, spec, table, "git", &has_git);
  base::Result<std::string_view, diag::Reported> branch =
      dep_string(bag, filename, spec, table, "branch", &has_branch);
  base::Result<std::string_view, diag::Reported> tag =
      dep_string(bag, filename, spec, table, "tag", &has_tag);
  base::Result<std::string_view, diag::Reported> rev =
      dep_string(bag, filename, spec, table, "rev", &has_rev);
  base::Result<std::string_view, diag::Reported> registry =
      dep_string(bag, filename, spec, table, "registry", &has_registry);
  if (path.is_err() || version.is_err() || git.is_err() || branch.is_err() ||
      tag.is_err() || rev.is_err() || registry.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const std::string_view path_text = std::move(path).unwrap();
  const std::string_view version_text = std::move(version).unwrap();
  const std::string_view git_text = std::move(git).unwrap();
  const std::string_view branch_text = std::move(branch).unwrap();
  const std::string_view tag_text = std::move(tag).unwrap();
  const std::string_view rev_text = std::move(rev).unwrap();
  const std::string_view registry_text = std::move(registry).unwrap();
  for (const auto& [key, node] : table) {
    const std::string_view field = key.str();
    if (field != "path" && field != "version" && field != "git" &&
        field != "branch" && field != "tag" && field != "rev" &&
        field != "registry") {
      bag.emit<i18n::Key::PkgDependencyUnknownField>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ManifestSemanticError, filename, spec, field);
      return base::make_err(diag::Reported{});
    }
    (void)node;
  }
  const u32 refs =
      (has_branch ? 1u : 0u) + (has_tag ? 1u : 0u) + (has_rev ? 1u : 0u);
  if (refs > 1) {
    bag.emit<i18n::Key::PkgDependencyTooManyRefs>(
        diag::Severity::Error, diag::Stage::Pkg,
        DiagCode::ManifestSemanticError, filename, spec);
    return base::make_err(diag::Reported{});
  }
  if (refs > 0 && !has_git) {
    bag.emit<i18n::Key::PkgDependencyRefWithoutRemote>(
        diag::Severity::Error, diag::Stage::Pkg,
        DiagCode::ManifestSemanticError, filename, spec);
    return base::make_err(diag::Reported{});
  }
  // A `path` beside `git` is the subpath inside the repository; beside
  // `version` it would be two sources at once.
  if (has_version && (has_git || has_path)) {
    bag.emit<i18n::Key::PkgDependencyTooManySources>(
        diag::Severity::Error, diag::Stage::Pkg,
        DiagCode::ManifestSemanticError, filename, spec);
    return base::make_err(diag::Reported{});
  }
  VersionReq version_req;
  if (has_version) {
    base::Result<VersionReq, VersionReqError> parsed =
        parse_version_req(version_text);
    if (parsed.is_err()) {
      bag.emit<i18n::Key::PkgDependencyBadVersion>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ManifestSemanticError, filename, spec);
      return base::make_err(diag::Reported{});
    }
    version_req = std::move(parsed).unwrap();
  }
  Dependency dep{
      .spec = copy_str(arena, spec),
      .owner = copy_str(arena, segments[0]),
      .suite = {},
      .member = {},
      .suite_glob = false,
      .source = DependencySource::Unspecified,
      .path = {},
      .version = {},
      .version_req = version_req,
      .git = {},
      .git_ref_kind = {},
      .git_ref = {},
      .registry = {},
      .name = copy_str(arena, spec),
  };
  if (parts == 3) {
    dep.suite = copy_str(arena, segments[1]);
    if (segments[2] == "*") {
      dep.suite_glob = true;
    } else {
      dep.member = copy_str(arena, segments[2]);
    }
  } else {
    // Two segments name a package directly; the suite stays empty so
    // selection can tell it from a package in a suite.
    dep.member = copy_str(arena, segments[1]);
  }
  if (has_git) {
    dep.source = DependencySource::Git;
    dep.git = copy_str(arena, git_text);
    if (has_path) {
      dep.path = copy_str(arena, path_text);
    }
    if (has_branch) {
      dep.git_ref_kind = "branch";
      dep.git_ref = copy_str(arena, branch_text);
    } else if (has_tag) {
      dep.git_ref_kind = "tag";
      dep.git_ref = copy_str(arena, tag_text);
    } else if (has_rev) {
      dep.git_ref_kind = "rev";
      dep.git_ref = copy_str(arena, rev_text);
    }
  } else if (has_version) {
    dep.source = DependencySource::Registry;
    dep.version = copy_str(arena, version_text);
  } else if (has_path) {
    dep.source = DependencySource::Path;
    dep.path = copy_str(arena, path_text);
  } else {
    dep.source = DependencySource::Unspecified;
  }
  if (has_registry) {
    dep.registry = copy_str(arena, registry_text);
  }
  return base::make_ok(dep);
}

}  // namespace

base::Result<void, ManifestError> verify_manifest(
    const PackageManifest& manifest) {
  if (manifest.name.empty()) {
    return base::make_err(ManifestError::EmptyName);
  }
  // A resolved manifest has a version and no inheritance marker left;
  // anything else crossed this boundary without inherit_from_suite.
  if (!manifest.has_version || manifest.version_from_suite ||
      manifest.owner_from_suite || manifest.license_from_suite) {
    return base::make_err(ManifestError::UnresolvedInheritance);
  }
  if (manifest.dependency_count > 0 && manifest.dependencies == nullptr) {
    return base::make_err(ManifestError::NullDependencyArray);
  }
  if (manifest.bin_count > 0 && manifest.bins == nullptr) {
    return base::make_err(ManifestError::NullBinArray);
  }
  if (manifest.modules.include_count > 0 &&
      manifest.modules.include == nullptr) {
    return base::make_err(ManifestError::NullModuleInclude);
  }
  if (manifest.modules.export_count > 0 &&
      manifest.modules.exports == nullptr) {
    return base::make_err(ManifestError::NullModuleExport);
  }
  for (u32 i = 0; i < manifest.dependency_count; ++i) {
    const Dependency& dependency = manifest.dependencies[i];
    if (dependency.name.empty()) {
      return base::make_err(ManifestError::EmptyDependencyName);
    }
    // Only a path source must name a directory; a registry entry may
    // leave the version to latest, and an empty table is for the
    // selection layer to accept or reject.
    if (dependency.source == DependencySource::Path &&
        dependency.path.empty()) {
      return base::make_err(ManifestError::EmptyDependencyPath);
    }
    if (dependency.source == DependencySource::Git && dependency.git.empty()) {
      return base::make_err(ManifestError::EmptyDependencyPath);
    }
  }
  for (u32 i = 0; i < manifest.bin_count; ++i) {
    // A bin name may be empty: it defaults to the package name.
    if (manifest.bins[i].path.empty()) {
      return base::make_err(ManifestError::EmptyBinPath);
    }
  }
  if (manifest.lib != nullptr) {
    // A lib name may be empty, like a bin name.
    if (manifest.lib->path.empty()) {
      return base::make_err(ManifestError::EmptyLibPath);
    }
  }
  for (u32 i = 0; i < manifest.modules.include_count; ++i) {
    if (manifest.modules.include[i].empty()) {
      return base::make_err(ManifestError::EmptyModuleEntry);
    }
  }
  for (u32 i = 0; i < manifest.modules.export_count; ++i) {
    if (manifest.modules.exports[i].empty()) {
      return base::make_err(ManifestError::EmptyModuleEntry);
    }
  }
  if (manifest.suite_only_count > 0 && manifest.suite_only == nullptr) {
    return base::make_err(ManifestError::NullSuiteOnly);
  }
  for (u32 i = 0; i < manifest.suite_only_count; ++i) {
    if (manifest.suite_only[i].empty()) {
      return base::make_err(ManifestError::EmptySuiteOnlyEntry);
    }
  }
  return base::make_ok();
}

void report_manifest_error(ManifestError error,
                           std::string_view name,
                           diag::DiagBag& bag) {
  std::string_view detail = "invalid manifest";
  switch (error) {
    case ManifestError::EmptyName: detail = "empty [package] name"; break;
    case ManifestError::NullDependencyArray:
      detail = "dependency count without a dependency array";
      break;
    case ManifestError::NullBinArray:
      detail = "bin count without a bin array";
      break;
    case ManifestError::NullModuleInclude:
      detail = "module include count without an include array";
      break;
    case ManifestError::NullModuleExport:
      detail = "module export count without an export array";
      break;
    case ManifestError::NullSuiteOnly:
      detail = "sealed spec count without a suite-only array";
      break;
    case ManifestError::EmptyDependencyName:
      detail = "dependency with an empty name";
      break;
    case ManifestError::EmptyDependencyPath:
      detail = "dependency with an empty path";
      break;
    case ManifestError::EmptyBinPath:
      detail = "bin target with an empty path";
      break;
    case ManifestError::EmptyLibPath:
      detail = "lib target with an empty path";
      break;
    case ManifestError::EmptyModuleEntry:
      detail = "module list with an empty entry";
      break;
    case ManifestError::EmptySuiteOnlyEntry:
      detail = "suite-only list with an empty entry";
      break;
    case ManifestError::UnresolvedInheritance:
      detail = "an inherited key was not resolved";
      break;
  }
  const u32 index = bag.emit<i18n::Key::PkgManifestInvalid>(
      diag::Severity::Error, diag::Stage::Pkg, DiagCode::ManifestSemanticError,
      name, detail);
  (void)index;
}

ManifestKind probe_manifest_kind(std::string_view bytes) {
  const toml::parse_result result = toml::parse(bytes);
  if (!result) {
    return ManifestKind::Unknown;
  }
  const toml::table& root = result.table();
  if (root.find("package") != root.end()) {
    return ManifestKind::Package;
  }
  if (root.find("suite") != root.end()) {
    return ManifestKind::Suite;
  }
  return ManifestKind::Unknown;
}

std::string_view suite_member_name(std::string_view path) {
  const usize slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

base::Result<void, SuiteError> verify_suite_manifest(
    const SuiteManifest& manifest) {
  if (manifest.name.empty()) {
    return base::make_err(SuiteError::EmptyName);
  }
  if (manifest.package_count > 0 && manifest.packages == nullptr) {
    return base::make_err(SuiteError::NullPackageArray);
  }
  // A member's address is its package name, the last segment of its
  // path, so a path has to end in one and names are unique
  // (ADR-0057).
  for (u32 i = 0; i < manifest.package_count; ++i) {
    const std::string_view path = manifest.packages[i];
    if (path.empty()) {
      return base::make_err(SuiteError::EmptyPackageEntry);
    }
    const std::string_view name = suite_member_name(path);
    if (name.empty()) {
      return base::make_err(SuiteError::BadPackageEntry);
    }
    for (u32 j = 0; j < i; ++j) {
      if (manifest.packages[j] == path) {
        return base::make_err(SuiteError::DuplicatePackageEntry);
      }
      if (suite_member_name(manifest.packages[j]) == name) {
        return base::make_err(SuiteError::DuplicatePackageName);
      }
    }
  }
  return base::make_ok();
}

void report_suite_error(SuiteError error,
                        std::string_view name,
                        diag::DiagBag& bag) {
  std::string_view detail = "invalid manifest";
  switch (error) {
    case SuiteError::EmptyName: detail = "empty [suite] name"; break;
    case SuiteError::NullPackageArray:
      detail = "package count without a package array";
      break;
    case SuiteError::EmptyPackageEntry:
      detail = "[suite] package list with an empty entry";
      break;
    case SuiteError::BadPackageEntry:
      detail = "[suite] package path that does not end in a name";
      break;
    case SuiteError::DuplicatePackageEntry:
      detail = "duplicate [suite] package entry";
      break;
    case SuiteError::DuplicatePackageName:
      detail = "duplicate [suite] package name";
      break;
  }
  const u32 index = bag.emit<i18n::Key::PkgManifestInvalid>(
      diag::Severity::Error, diag::Stage::Pkg, DiagCode::ManifestSemanticError,
      name, detail);
  (void)index;
}

base::Result<void, InheritError> inherit_from_suite(
    PackageManifest& member,
    const SuiteManifest& suite) {
  if (member.owner_from_suite) {
    member.owner = suite.owner;
    member.owner_from_suite = false;
  }
  if (member.license_from_suite) {
    member.license = suite.license;
    member.license_from_suite = false;
  }
  if (member.version_from_suite) {
    if (!suite.has_version) {
      return base::make_err(InheritError::SuiteVersionMissing);
    }
    member.version = suite.version;
    member.version_from_suite = false;
    member.has_version = true;
  }
  return base::make_ok();
}

void report_inherit_error(InheritError error,
                          std::string_view name,
                          diag::DiagBag& bag) {
  std::string_view detail = "invalid manifest";
  switch (error) {
    case InheritError::SuiteVersionMissing:
      detail = "inherits a version, but the suite declares none";
      break;
  }
  const u32 index = bag.emit<i18n::Key::PkgManifestInvalid>(
      diag::Severity::Error, diag::Stage::Pkg, DiagCode::ManifestSemanticError,
      name, detail);
  (void)index;
}

// Reports a TOML syntax failure with its source span; only syntax errors
// have a position to report.
diag::Reported toml_syntax_error(diag::DiagBag& bag,
                                 std::string_view bytes,
                                 source::FileId file,
                                 const toml::parse_error& error) {
  const u32 index = bag.emit<i18n::Key::PkgTomlSyntaxError>(
      diag::Severity::Error, diag::Stage::Pkg, DiagCode::ManifestSyntaxError,
      toml_span(bytes, file, error.source()), error.description());
  (void)index;
  return diag::Reported{};
}

base::Result<SuiteManifest, diag::Reported> suite_semantic_error(
    diag::DiagBag& bag,
    std::string_view filename,
    std::string_view message) {
  // Same shape as semantic_error, for the suite parser: semantic errors
  // name the manifest in the message and carry no span.
  const u32 index = bag.emit<i18n::Key::PkgManifestInvalid>(
      diag::Severity::Error, diag::Stage::Pkg, DiagCode::ManifestSemanticError,
      filename, message);
  (void)index;
  return base::make_err(diag::Reported{});
}

base::Result<PackageManifest, diag::Reported> parse_manifest(
    std::string_view bytes,
    std::string_view filename,
    source::FileId file,
    diag::DiagBag& bag,
    mem::Arena& arena) {
  toml::parse_result result = toml::parse(bytes, filename);
  if (!result) {
    return base::make_err(toml_syntax_error(bag, bytes, file, result.error()));
  }

  const toml::table& root = result.table();
  const auto pkg_it = root.find("package");
  if (pkg_it == root.end() || !pkg_it->second.is_table()) {
    // A suite manifest names the same file, so reaching for the wrong
    // parser names the kind it found rather than just the table absent.
    const auto suite_it = root.find("suite");
    if (suite_it != root.end() && suite_it->second.is_table()) {
      return semantic_error(bag, filename,
                            "is a suite manifest, not a package manifest");
    }
    return semantic_error(bag, filename, "missing [package] table");
  }
  const toml::table* const pkg_table = pkg_it->second.as_table();

  const auto name_it = pkg_table->find("name");
  if (name_it == pkg_table->end()) {
    return semantic_error(bag, filename, "missing [package] name");
  }
  const auto name = name_it->second.value<std::string_view>();
  if (!name.has_value() || name->empty()) {
    return semantic_error(bag, filename, "[package] name must be a string");
  }

  base::Result<ManifestVersionField, diag::Reported> version_field =
      manifest_version(bag, filename, *pkg_table,
                       "[package] version must be a string or { suite = true }",
                       "invalid [package] version");
  if (version_field.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const ManifestVersionField version = std::move(version_field).unwrap();
  if (!version.present && !version.from_suite) {
    return semantic_error(bag, filename, "missing [package] version");
  }

  base::Result<InheritedString, diag::Reported> owner_field =
      inherited_string(bag, filename, arena, *pkg_table, "owner",
                       "[package] owner must be a string or { suite = true }");
  if (owner_field.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const InheritedString owner = std::move(owner_field).unwrap();

  base::Result<InheritedString, diag::Reported> license_field =
      inherited_string(
          bag, filename, arena, *pkg_table, "license",
          "[package] license must be a string or { suite = true }");
  if (license_field.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const InheritedString license = std::move(license_field).unwrap();
  if (!license.present && !license.from_suite) {
    return semantic_error(bag, filename, "missing [package] license");
  }

  std::string_view edition;
  const auto edition_it = pkg_table->find("edition");
  if (edition_it != pkg_table->end()) {
    const auto edition_text = edition_it->second.value<std::string_view>();
    if (!edition_text.has_value()) {
      return semantic_error(bag, filename,
                            "[package] edition must be a string");
    }
    edition = copy_str(arena, *edition_text);
  }

  // Two passes so the dependency array needs no growth logic.
  u32 dependency_count = 0;
  const toml::table* deps_table = nullptr;
  const auto deps_it = root.find("dependencies");
  if (deps_it != root.end()) {
    if (!deps_it->second.is_table()) {
      return semantic_error(bag, filename, "[dependencies] must be a table");
    }
    deps_table = deps_it->second.as_table();
    for (const auto& [key, node] : *deps_table) {
      (void)key;
      (void)node;
      ++dependency_count;
    }
  }

  Dependency* const dependencies =
      dependency_count > 0
          ? static_cast<Dependency*>(arena.alloc(
                sizeof(Dependency) * dependency_count, alignof(Dependency)))
          : nullptr;
  u32 filled = 0;
  if (deps_table != nullptr) {
    for (const auto& [key, node] : *deps_table) {
      const std::string_view dep_name = key.str();
      base::Result<Dependency, diag::Reported> parsed =
          parse_dependency(bag, filename, arena, dep_name, node);
      if (parsed.is_err()) {
        return base::make_err(diag::Reported{});
      }
      dependencies[filled++] = std::move(parsed).unwrap();
    }
  }

  u32 bin_count = 0;
  const toml::array* bins_array = nullptr;
  const auto bins_it = root.find("bin");
  if (bins_it != root.end()) {
    if (!bins_it->second.is_array()) {
      return semantic_error(bag, filename,
                            "[[bin]] must be an array of tables");
    }
    bins_array = bins_it->second.as_array();
    bin_count = static_cast<u32>(bins_array->size());
  }

  BinTarget* const bins =
      bin_count > 0 ? static_cast<BinTarget*>(arena.alloc(
                          sizeof(BinTarget) * bin_count, alignof(BinTarget)))
                    : nullptr;
  u32 bin_filled = 0;
  if (bins_array != nullptr) {
    for (const toml::node& node : *bins_array) {
      if (!node.is_table()) {
        return semantic_error(bag, filename, "[[bin]] entries must be tables");
      }
      const toml::table* const bin_table = node.as_table();
      const auto path_it = bin_table->find("path");
      if (path_it == bin_table->end()) {
        return semantic_error(bag, filename, "[[bin]] entry needs a path");
      }
      const auto bin_path = path_it->second.value<std::string_view>();
      if (!bin_path.has_value() || bin_path->empty()) {
        return semantic_error(bag, filename, "[[bin]] path must be a string");
      }
      std::string_view bin_name;
      const auto name_it = bin_table->find("name");
      if (name_it != bin_table->end()) {
        const auto name_value = name_it->second.value<std::string_view>();
        if (!name_value.has_value() || name_value->empty()) {
          return semantic_error(bag, filename, "[[bin]] name must be a string");
        }
        bin_name = copy_str(arena, *name_value);
      }
      bins[bin_filled++] = BinTarget{
          .name = bin_name,
          .path = copy_str(arena, *bin_path),
      };
    }
  }

  // The [lib] table is optional and singular: one library per
  // package, naming its root module the way a bin names its entry.
  LibTarget* lib = nullptr;
  const auto lib_it = root.find("lib");
  if (lib_it != root.end()) {
    if (!lib_it->second.is_table()) {
      return semantic_error(bag, filename, "[lib] must be a table");
    }
    const toml::table* const lib_table = lib_it->second.as_table();
    const auto lib_path_it = lib_table->find("path");
    if (lib_path_it == lib_table->end()) {
      return semantic_error(bag, filename, "[lib] entry needs a path");
    }
    const auto lib_path = lib_path_it->second.value<std::string_view>();
    if (!lib_path.has_value() || lib_path->empty()) {
      return semantic_error(bag, filename, "[lib] path must be a string");
    }
    std::string_view lib_name;
    const auto lib_name_it = lib_table->find("name");
    if (lib_name_it != lib_table->end()) {
      const auto lib_name_value = lib_name_it->second.value<std::string_view>();
      if (!lib_name_value.has_value() || lib_name_value->empty()) {
        return semantic_error(bag, filename, "[lib] name must be a string");
      }
      lib_name = copy_str(arena, *lib_name_value);
    }
    lib = static_cast<LibTarget*>(
        arena.alloc(sizeof(LibTarget), alignof(LibTarget)));
    *lib = LibTarget{
        .name = lib_name,
        .path = copy_str(arena, *lib_path),
    };
  }

  // The [modules] table is optional; an absent table selects every
  // discovered source file, keeping starter packages manifest-light.
  ModuleSet modules;
  const auto modules_it = root.find("modules");
  if (modules_it != root.end()) {
    if (!modules_it->second.is_table()) {
      return semantic_error(bag, filename, "[modules] must be a table");
    }
    const toml::table* const modules_table = modules_it->second.as_table();
    const auto include_it = modules_table->find("include");
    if (include_it != modules_table->end()) {
      if (!include_it->second.is_array()) {
        return semantic_error(bag, filename,
                              "[modules] include must be an array");
      }
      const toml::array* const include_array = include_it->second.as_array();
      const u32 include_count = static_cast<u32>(include_array->size());
      std::string_view* const include =
          include_count > 0 ? static_cast<std::string_view*>(arena.alloc(
                                  sizeof(std::string_view) * include_count,
                                  alignof(std::string_view)))
                            : nullptr;
      u32 include_filled = 0;
      for (const toml::node& node : *include_array) {
        const auto entry = node.value<std::string_view>();
        if (!entry.has_value() || entry->empty()) {
          return semantic_error(bag, filename,
                                "[modules] include entries must be strings");
        }
        if (*entry == "*") {
          // Repeating the wildcard says nothing new, so it is not a
          // duplicate the way repeating a name is.
          modules.wildcard = true;
          continue;
        }
        for (u32 i = 0; i < include_filled; ++i) {
          if (include[i] == *entry) {
            return semantic_error(bag, filename,
                                  "duplicate [modules] include entry");
          }
        }
        include[include_filled++] = copy_str(arena, *entry);
      }
      modules.include = include;
      modules.include_count = include_filled;
    } else {
      modules.wildcard = true;
    }
    const auto export_it = modules_table->find("export");
    if (export_it != modules_table->end()) {
      if (!export_it->second.is_array()) {
        return semantic_error(bag, filename,
                              "[modules] export must be an array");
      }
      const toml::array* const export_array = export_it->second.as_array();
      const u32 export_count = static_cast<u32>(export_array->size());
      std::string_view* const exports =
          export_count > 0 ? static_cast<std::string_view*>(arena.alloc(
                                 sizeof(std::string_view) * export_count,
                                 alignof(std::string_view)))
                           : nullptr;
      u32 export_filled = 0;
      for (const toml::node& node : *export_array) {
        const auto entry = node.value<std::string_view>();
        if (!entry.has_value() || entry->empty()) {
          return semantic_error(bag, filename,
                                "[modules] export entries must be strings");
        }
        exports[export_filled++] = copy_str(arena, *entry);
      }
      modules.exports = exports;
      modules.export_count = export_filled;
    }
  } else {
    modules.wildcard = true;
  }

  // The [spec] table names the specs this package seals to its suite,
  // so only packages of that suite may implement them (ADR-0053).
  // `implement` is the implementing side's reserved key; until its
  // slice accepts it, naming it is an error rather than a no-op.
  const std::string_view* suite_only = nullptr;
  u32 suite_only_count = 0;
  const auto spec_it = root.find("spec");
  if (spec_it != root.end()) {
    if (!spec_it->second.is_table()) {
      return semantic_error(bag, filename, "[spec] must be a table");
    }
    const toml::table* const spec_table = spec_it->second.as_table();
    for (const auto& [key, node] : *spec_table) {
      const std::string_view field = key.str();
      if (field == "suite-only") {
        if (!node.is_array()) {
          const u32 index = bag.emit<i18n::Key::PkgSpecSuiteOnlyNotStrings>(
              diag::Severity::Error, diag::Stage::Pkg,
              DiagCode::ManifestSemanticError, filename);
          (void)index;
          return base::make_err(diag::Reported{});
        }
        const toml::array* const array = node.as_array();
        const u32 count = static_cast<u32>(array->size());
        std::string_view* const names =
            count > 0 ? static_cast<std::string_view*>(
                            arena.alloc(sizeof(std::string_view) * count,
                                        alignof(std::string_view)))
                      : nullptr;
        u32 filled = 0;
        for (const toml::node& entry : *array) {
          const auto text = entry.value<std::string_view>();
          if (!text.has_value() || text->empty()) {
            const u32 index = bag.emit<i18n::Key::PkgSpecSuiteOnlyNotStrings>(
                diag::Severity::Error, diag::Stage::Pkg,
                DiagCode::ManifestSemanticError, filename);
            (void)index;
            return base::make_err(diag::Reported{});
          }
          bool duplicate = false;
          for (u32 i = 0; i < filled; ++i) {
            if (names[i] == *text) {
              duplicate = true;
              break;
            }
          }
          if (duplicate) {
            const u32 index = bag.emit<i18n::Key::PkgSpecSuiteOnlyDuplicate>(
                diag::Severity::Error, diag::Stage::Pkg,
                DiagCode::ManifestSemanticError, filename, *text);
            (void)index;
            return base::make_err(diag::Reported{});
          }
          names[filled++] = copy_str(arena, *text);
        }
        suite_only = names;
        suite_only_count = filled;
        continue;
      }
      if (field == "implement") {
        const u32 index = bag.emit<i18n::Key::PkgSpecImplementReserved>(
            diag::Severity::Error, diag::Stage::Pkg,
            DiagCode::ManifestSemanticError, filename);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      const u32 index = bag.emit<i18n::Key::PkgSpecUnknownKey>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ManifestSemanticError, filename, field);
      (void)index;
      return base::make_err(diag::Reported{});
    }
  }

  return base::make_ok(PackageManifest{
      .name = copy_str(arena, *name),
      .version = version.version,
      .has_version = version.present,
      .version_from_suite = version.from_suite,
      .edition = edition,
      .owner = owner.text,
      .owner_from_suite = owner.from_suite,
      .license = license.text,
      .license_from_suite = license.from_suite,
      .dependencies = dependencies,
      .dependency_count = dependency_count,
      .bins = bins,
      .bin_count = bin_count,
      .lib = lib,
      .modules = modules,
      .suite_only = suite_only,
      .suite_only_count = suite_only_count,
  });
}

base::Result<SuiteManifest, diag::Reported> parse_suite_manifest(
    std::string_view bytes,
    std::string_view filename,
    source::FileId file,
    diag::DiagBag& bag,
    mem::Arena& arena) {
  toml::parse_result result = toml::parse(bytes, filename);
  if (!result) {
    return base::make_err(toml_syntax_error(bag, bytes, file, result.error()));
  }

  const toml::table& root = result.table();
  if (root.find("package") != root.end()) {
    return suite_semantic_error(bag, filename,
                                "is a package manifest, not a suite manifest");
  }
  const auto suite_it = root.find("suite");
  if (suite_it == root.end() || !suite_it->second.is_table()) {
    return suite_semantic_error(bag, filename, "missing [suite] table");
  }
  const toml::table* const suite_table = suite_it->second.as_table();

  base::Result<InheritedString, diag::Reported> owner_field =
      inherited_string(bag, filename, arena, *suite_table, "owner",
                       "[suite] owner must be a string");
  if (owner_field.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const InheritedString owner = std::move(owner_field).unwrap();
  if (owner.from_suite) {
    return suite_semantic_error(bag, filename,
                                "[suite] owner cannot be inherited");
  }

  base::Result<ManifestVersionField, diag::Reported> version_field =
      manifest_version(bag, filename, *suite_table,
                       "[suite] version must be a string",
                       "[suite] version is not a version");
  if (version_field.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const ManifestVersionField version = std::move(version_field).unwrap();
  if (version.from_suite) {
    return suite_semantic_error(bag, filename,
                                "[suite] version cannot be inherited");
  }

  base::Result<InheritedString, diag::Reported> license_field =
      inherited_string(bag, filename, arena, *suite_table, "license",
                       "[suite] license must be a string");
  if (license_field.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const InheritedString license = std::move(license_field).unwrap();
  if (license.from_suite) {
    return suite_semantic_error(bag, filename,
                                "[suite] license cannot be inherited");
  }

  const auto name_it = suite_table->find("name");
  if (name_it == suite_table->end()) {
    return suite_semantic_error(bag, filename, "missing [suite] name");
  }
  const auto name = name_it->second.value<std::string_view>();
  if (!name.has_value() || name->empty()) {
    return suite_semantic_error(bag, filename, "[suite] name must be a string");
  }

  if (!license.present) {
    return suite_semantic_error(bag, filename, "missing [suite] license");
  }

  const auto packages_it = suite_table->find("packages");
  if (packages_it == suite_table->end()) {
    return suite_semantic_error(bag, filename, "missing [suite] packages");
  }
  if (!packages_it->second.is_array()) {
    return suite_semantic_error(bag, filename,
                                "[suite] packages must be an array");
  }
  const toml::array* const packages_array = packages_it->second.as_array();
  const u32 package_count = static_cast<u32>(packages_array->size());
  std::string_view* const packages =
      package_count > 0 ? static_cast<std::string_view*>(arena.alloc(
                              sizeof(std::string_view) * package_count,
                              alignof(std::string_view)))
                        : nullptr;
  u32 filled = 0;
  for (const toml::node& node : *packages_array) {
    const auto entry = node.value<std::string_view>();
    if (!entry.has_value() || entry->empty()) {
      return suite_semantic_error(bag, filename,
                                  "[suite] package entries must be strings");
    }
    for (u32 i = 0; i < filled; ++i) {
      if (packages[i] == *entry) {
        return suite_semantic_error(bag, filename,
                                    "duplicate [suite] package entry");
      }
    }
    packages[filled++] = copy_str(arena, *entry);
  }

  return base::make_ok(SuiteManifest{
      .owner = owner.text,
      .name = copy_str(arena, *name),
      .version = version.version,
      .has_version = version.present,
      .license = license.text,
      .packages = packages,
      .package_count = filled,
  });
}

// Trims ASCII whitespace from both ends; fragments arrive raw from argv.
std::string_view trim_flag(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

base::Result<Dependency, diag::Reported> parse_dependency_flag(
    diag::DiagBag& bag,
    mem::Arena& arena,
    std::string_view fragment) {
  constexpr std::string_view filename = "command line";
  std::string_view body = trim_flag(fragment);
  std::string table;
  const usize eq = body.find('=');
  if (eq == std::string_view::npos) {
    // A bare specifier selects the embedded member with no source.
    table = "\"";
    table += std::string(body);
    table += "\" = {}";
  } else {
    std::string_view key = trim_flag(body.substr(0, eq));
    std::string_view value = trim_flag(body.substr(eq + 1));
    if (value.empty()) {
      bag.emit<i18n::Key::PkgDependencyNotASpecifier>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ManifestSemanticError, filename, fragment);
      return base::make_err(diag::Reported{});
    }
    // Slashes need quoting for TOML, so a bare key is quoted here
    // rather than at every call site.
    if (key.empty() || key.front() != '"') {
      table = "\"";
      table += std::string(key);
      table += "\" = ";
    } else {
      table = std::string(key);
      table += " = ";
    }
    table += std::string(value);
  }
  toml::parse_result parsed =
      toml::parse("[dependencies]\n" + table + "\n", filename);
  if (!parsed) {
    bag.emit<i18n::Key::PkgDependencyDoesNotParse>(
        diag::Severity::Error, diag::Stage::Pkg, DiagCode::ManifestSyntaxError,
        filename, fragment);
    return base::make_err(diag::Reported{});
  }
  const toml::table& root = parsed.table();
  const auto deps_it = root.find("dependencies");
  if (deps_it == root.end() || !deps_it->second.is_table() ||
      deps_it->second.as_table()->empty()) {
    bag.emit<i18n::Key::PkgDependencyNotASpecifier>(
        diag::Severity::Error, diag::Stage::Pkg,
        DiagCode::ManifestSemanticError, filename, fragment);
    return base::make_err(diag::Reported{});
  }
  const toml::table* const deps = deps_it->second.as_table();
  if (deps->size() != 1) {
    bag.emit<i18n::Key::PkgDependencyTooManyEntries>(
        diag::Severity::Error, diag::Stage::Pkg,
        DiagCode::ManifestSemanticError, filename, fragment);
    return base::make_err(diag::Reported{});
  }
  const auto entry = deps->begin();
  return parse_dependency(bag, filename, arena, entry->first.str(),
                          entry->second);
}

}  // namespace pkg
