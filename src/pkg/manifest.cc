// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/manifest.h"

#include <optional>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#pragma clang diagnostic ignored "-Wswitch"
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
#pragma clang diagnostic pop

namespace pkg {

namespace {

// Diagnostic codes 2000-2099 are reserved for manifest errors.
constexpr u32 kManifestSyntaxError = 2000;
constexpr u32 kManifestSemanticError = 2001;

// Copies bytes into the arena for model views.
std::string_view copy_str(mem::Arena& arena, std::string_view bytes) {
  if (bytes.empty()) {
    return {};
  }
  char* const mem =
      static_cast<char*>(arena.alloc(bytes.size(), alignof(char)));
  for (usize i = 0; i < bytes.size(); ++i) {
    mem[i] = bytes[i];
  }
  return {mem, bytes.size()};
}

// Converts a 1-based toml line/column into a byte offset, clamped.
u32 line_col_to_offset(std::string_view bytes, u32 line, u32 column) {
  u32 offset = 0;
  for (u32 current = 1; current < line && offset < bytes.size(); ++offset) {
    if (bytes[offset] == '\n') {
      ++current;
    }
  }
  offset += (column > 0 ? column - 1 : 0);
  if (offset > bytes.size()) {
    offset = static_cast<u32>(bytes.size());
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

diag::Fallible<PackageManifest> semantic_error(diag::DiagBag& bag,
                                               std::string_view filename,
                                               std::string_view message) {
  // Semantic errors carry the manifest path in the message; spans attach
  // once lowering tracks node positions (a later phase).
  const u32 index = bag.emit(diag::Severity::Error, kManifestSemanticError,
                             "manifest '{}': {}", filename, message);
  (void)index;
  return base::make_err(diag::Fatal{});
}

}  // namespace

diag::Fallible<PackageManifest> parse_manifest(std::string_view bytes,
                                               std::string_view filename,
                                               source::FileId file,
                                               diag::DiagBag& bag,
                                               mem::Arena& arena) {
  toml::parse_result result = toml::parse(bytes, filename);
  if (!result) {
    const toml::parse_error& error = result.error();
    const u32 index = bag.emit(diag::Severity::Error, kManifestSyntaxError,
                               toml_span(bytes, file, error.source()),
                               "TOML syntax error: {}", error.description());
    (void)index;
    return base::make_err(diag::Fatal{});
  }

  const toml::table& root = result.table();
  const auto pkg_it = root.find("package");
  if (pkg_it == root.end() || !pkg_it->second.is_table()) {
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

  const auto version_it = pkg_table->find("version");
  if (version_it == pkg_table->end()) {
    return semantic_error(bag, filename, "missing [package] version");
  }
  const auto version_text = version_it->second.value<std::string_view>();
  if (!version_text.has_value()) {
    return semantic_error(bag, filename, "[package] version must be a string");
  }
  base::Result<Version, VersionError> version = parse_version(*version_text);
  if (version.is_err()) {
    return semantic_error(bag, filename, "invalid [package] version");
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
      if (!node.is_table()) {
        // Registry-style `foo = "1.0"` has no path to follow.
        bag.emit(diag::Severity::Error, kManifestSemanticError,
                 "manifest '{}': dependency '{}' needs {{ path = ... }}; "
                 "registry dependencies are not supported",
                 filename, dep_name);
        return base::make_err(diag::Fatal{});
      }
      const toml::table* const dep_table = node.as_table();
      const auto path_it = dep_table->find("path");
      if (path_it == dep_table->end()) {
        bag.emit(diag::Severity::Error, kManifestSemanticError,
                 "manifest '{}': dependency '{}' needs {{ path = ... }}; "
                 "registry dependencies are not supported",
                 filename, dep_name);
        return base::make_err(diag::Fatal{});
      }
      const auto path = path_it->second.value<std::string_view>();
      if (!path.has_value() || path->empty()) {
        return semantic_error(bag, filename,
                              "dependency path must be a string");
      }
      dependencies[filled++] = Dependency{
          .name = copy_str(arena, dep_name),
          .path = copy_str(arena, *path),
      };
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

  return base::make_ok(PackageManifest{
      .name = copy_str(arena, *name),
      .version = std::move(version).unwrap(),
      .edition = edition,
      .dependencies = dependencies,
      .dependency_count = dependency_count,
      .bins = bins,
      .bin_count = bin_count,
      .modules = modules,
  });
}

}  // namespace pkg
