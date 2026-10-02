// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/modules.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pkg/arena_copy.h"
#include "pkg/diag_code.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

namespace {

// Strips a root prefix plus any following separators. False when `path`
// lies outside `root`; the prefix must end on a separator boundary, so
// `/proj` does not match `/project/a.al`. An empty remainder means the
// path *is* the root, which callers treat as "not a source file".
bool relative_to(std::string_view path,
                 std::string_view root,
                 std::string_view& out) {
  // A `.` root normalizes away: `Path(".").join("a.al")` is `a.al`, so
  // there is no textual prefix left to strip and every discovered path
  // is already root-relative.
  if (root == "." || root.empty()) {
    out = path;
    return true;
  }
  if (path.size() < root.size() || path.substr(0, root.size()) != root) {
    return false;
  }
  std::string_view rest = path.substr(root.size());
  if (!rest.empty() && rest.front() != '/' && rest.front() != '\\') {
    return false;
  }
  while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) {
    rest.remove_prefix(1);
  }
  out = rest;
  return true;
}

// Derives a module name from a root-relative `.al` path (`utils/io`
// from `utils/io.al`); native separators become slashes. Empty for
// non-source paths.
std::string module_name_of(std::string_view relative) {
  constexpr std::string_view suffix = ".al";
  if (relative.size() <= suffix.size() ||
      relative.substr(relative.size() - suffix.size()) != suffix) {
    return {};
  }
  std::string name(relative.substr(0, relative.size() - suffix.size()));
  for (char& c : name) {
    if (c == '\\') {
      c = '/';
    }
  }
  return name;
}

}  // namespace

base::Result<std::vector<ModuleFile>, diag::Reported> resolve_module_files(
    const PackageManifest& manifest,
    std::string_view root,
    const std::vector<source::FileId>& files,
    source::SourceManager& sources,
    diag::DiagBag& bag,
    mem::Arena& arena) {
  // Entry validation: manifests built directly (rather than parsed)
  // must be structurally sound before the selection below reads them.
  base::Result<void, ManifestError> verified = verify_manifest(manifest);
  if (verified.is_err()) {
    report_manifest_error(std::move(verified).unwrap_err(), root, bag);
    return base::make_err(diag::Reported{});
  }
  std::vector<ModuleFile> selected;
  // Dedup by file id, not by name: two spellings of one path resolve to
  // one file, and a name collision between two files is a manifest
  // error rather than a silent drop.
  auto add_module = [&](const std::string& name, source::FileId id) -> bool {
    for (const ModuleFile& prior : selected) {
      if (prior.id == id) {
        return true;
      }
    }
    for (const ModuleFile& prior : selected) {
      if (prior.name == name) {
        const u32 index = bag.emit<i18n::Key::PkgModuleSelectedTwice>(
            diag::Severity::Error, diag::Stage::Pkg,
            DiagCode::ModulesSemanticError, diag::Span{}, name);
        (void)index;
        return false;
      }
    }
    selected.push_back({copy_str(arena, name), id});
    return true;
  };

  for (u32 i = 0; i < manifest.modules.include_count; ++i) {
    const std::string_view entry = manifest.modules.include[i];
    base::Result<path::Path, path::PathError> candidate_root =
        path::Path::from_native(root);
    if (candidate_root.is_err()) {
      continue;
    }
    const path::Path candidate =
        std::move(candidate_root).unwrap().join(std::string(entry) + ".al");
    source::FileId found = source::UNKNOWN_FILE;
    for (source::FileId id : files) {
      const std::optional<std::string_view> name = sources.name(id);
      if (name.has_value() && *name == candidate.as_view()) {
        found = id;
        break;
      }
    }
    if (found == source::UNKNOWN_FILE) {
      const u32 index = bag.emit<i18n::Key::PkgModulesIncludeHasNoFile>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ModulesSemanticError, diag::Span{}, entry);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    // The name comes from the resolved path, not the manifest entry, so
    // an entry that only normalizes to a valid spelling (`./util`,
    // `sub//util`) still yields the module the wildcard would give it.
    // Otherwise the two spellings register the same file under
    // different names and it compiles twice.
    std::string_view relative;
    if (!relative_to(candidate.as_view(), root, relative)) {
      const u32 index = bag.emit<i18n::Key::PkgModulesIncludeEscapesPackage>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ModulesSemanticError, diag::Span{}, entry);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    const std::string name = module_name_of(relative);
    if (name.empty()) {
      const u32 index = bag.emit<i18n::Key::PkgModulesIncludeNotAModulePath>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ModulesSemanticError, diag::Span{}, entry);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    if (!add_module(name, found)) {
      return base::make_err(diag::Reported{});
    }
  }

  if (manifest.modules.wildcard) {
    for (source::FileId id : files) {
      const std::optional<std::string_view> name = sources.name(id);
      if (!name.has_value()) {
        continue;
      }
      std::string_view relative;
      if (!relative_to(*name, root, relative) || relative.empty()) {
        continue;
      }
      const std::string name_str = module_name_of(relative);
      if (name_str.empty()) {
        continue;
      }
      if (!add_module(name_str, id)) {
        return base::make_err(diag::Reported{});
      }
    }
  }
  for (source::FileId id : files) {
    bool taken = false;
    for (const ModuleFile& entry : selected) {
      if (entry.id == id) {
        taken = true;
        break;
      }
    }
    if (!taken) {
      const u32 index = bag.emit<i18n::Key::PkgSourceFileNotSelected>(
          diag::Severity::Warning, diag::Stage::Pkg,
          DiagCode::ModulesUnselectedFile,
          sources.name(id).value_or(std::string_view{"[unknown file]"}));
      (void)index;
    }
  }
  return base::make_ok(std::move(selected));
}

}  // namespace pkg
