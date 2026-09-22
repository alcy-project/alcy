// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/modules.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "path/path.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

namespace {

constexpr u32 kModulesSemanticError = 2001;
constexpr u32 kModulesUnselectedFile = 2002;

// Copies bytes into arena storage for name views.
std::string_view copy_str(mem::Arena& arena, std::string_view bytes) {
  char* const out =
      static_cast<char*>(arena.alloc(bytes.size() + 1, alignof(char)));
  usize i = 0;
  for (char c : bytes) {
    out[i++] = c;
  }
  out[i] = '\0';
  return std::string_view(out, bytes.size());
}

// Strips a root prefix plus one separator; empty when outside root.
std::string_view relative_to(std::string_view path, std::string_view root) {
  if (path.size() < root.size() || path.substr(0, root.size()) != root) {
    return {};
  }
  std::string_view rest = path.substr(root.size());
  while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) {
    rest.remove_prefix(1);
  }
  return rest;
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

diag::Fallible<std::vector<ModuleFile>> resolve_module_files(
    const PackageManifest& manifest,
    std::string_view root,
    const std::vector<source::FileId>& files,
    source::SourceManager& sources,
    diag::DiagBag& bag,
    mem::Arena& arena) {
  std::vector<ModuleFile> selected;
  auto add_module = [&](const std::string& name, source::FileId id) {
    for (const ModuleFile& prior : selected) {
      if (prior.name == name) {
        return;
      }
    }
    selected.push_back({copy_str(arena, name), id});
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
    source::FileId found = source::kUnknownFile;
    for (source::FileId id : files) {
      if (sources.name(id) == candidate.as_view()) {
        found = id;
        break;
      }
    }
    if (found == source::kUnknownFile) {
      const u32 index =
          bag.emit(diag::Severity::Error, kModulesSemanticError, diag::Span{},
                   "manifest [modules] include '{}' has no file", entry);
      (void)index;
      return base::make_err(diag::Fatal{});
    }
    add_module(std::string(entry), found);
  }

  if (manifest.modules.wildcard) {
    for (source::FileId id : files) {
      const std::string_view relative = relative_to(sources.name(id), root);
      if (relative.empty()) {
        continue;
      }
      const std::string name = module_name_of(relative);
      if (name.empty()) {
        continue;
      }
      add_module(name, id);
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
      const u32 index =
          bag.emit(diag::Severity::Warning, kModulesUnselectedFile,
                   "source file '{}' is not in [modules]", sources.name(id));
      (void)index;
    }
  }
  return base::make_ok(std::move(selected));
}

}  // namespace pkg
