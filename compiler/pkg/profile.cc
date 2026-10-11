// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/profile.h"

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
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
#include "toml++/impl/node.hpp"
#include "toml++/impl/parse_error.hpp"
#include "toml++/impl/parse_result.hpp"
#include "toml++/impl/parser.hpp"
#include "toml++/impl/table.hpp"
#include "pkg/diag_code.h"
// clang-format on

namespace pkg {

namespace {

// Reads one table's keys. A key this parser does not know is ignored,
// like an unknown key in alcy.toml; a known key with the wrong type is
// an error.
base::Result<ProfileSettings, diag::Reported> parse_settings(
    const toml::table& table,
    std::string_view filename,
    diag::DiagBag& bag) {
  ProfileSettings settings;
  for (const auto& [key, node] : table) {
    const std::string_view name = key.str();
    if (name == "optimize") {
      const auto flag = node.value<bool>();
      if (!flag.has_value()) {
        const u32 index = bag.emit<i18n::Key::PkgProfileOptimizeNotBool>(
            diag::Severity::Error, diag::Stage::Pkg,
            DiagCode::ProfileSemanticError, filename);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      settings.optimize = *flag;
    }
    if (name == "on-panic") {
      const auto text = node.value<std::string_view>();
      const std::optional<OnPanic> value =
          text.has_value() ? on_panic_from_name(*text) : std::nullopt;
      if (!value.has_value()) {
        const u32 index = bag.emit<i18n::Key::PkgProfileOnPanicUnknown>(
            diag::Severity::Error, diag::Stage::Pkg,
            DiagCode::ProfileSemanticError, filename);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      settings.on_panic = *value;
    }
  }
  return base::make_ok(settings);
}

}  // namespace

std::optional<OnPanic> on_panic_from_name(std::string_view name) {
  if (name == "abort") {
    return OnPanic::Abort;
  }
  if (name == "backtrace") {
    return OnPanic::Backtrace;
  }
  return std::nullopt;
}

base::Result<Profiles, diag::Reported> parse_profile(std::string_view bytes,
                                                     std::string_view filename,
                                                     source::FileId /*file*/,
                                                     diag::DiagBag& bag,
                                                     mem::Arena& arena) {
  toml::parse_result result = toml::parse(bytes, filename);
  if (!result) {
    // The profile file is read without a source location, so the
    // message names the file rather than pointing into it.
    const toml::parse_error& error = result.error();
    const u32 index = bag.emit<i18n::Key::PkgProfileTomlSyntaxError>(
        diag::Severity::Error, diag::Stage::Pkg, DiagCode::ProfileSyntaxError,
        filename, error.description());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const toml::table& root = result.table();
  ProfileTable* tables = nullptr;
  if (!root.empty()) {
    tables = static_cast<ProfileTable*>(
        arena.alloc(sizeof(ProfileTable) * root.size(), alignof(ProfileTable)));
  }
  u32 filled = 0;
  for (const auto& [key, node] : root) {
    if (!node.is_table()) {
      const u32 index = bag.emit<i18n::Key::PkgProfileKeyNotTable>(
          diag::Severity::Error, diag::Stage::Pkg,
          DiagCode::ProfileSemanticError, filename, key.str());
      (void)index;
      return base::make_err(diag::Reported{});
    }
    base::Result<ProfileSettings, diag::Reported> settings =
        parse_settings(*node.as_table(), filename, bag);
    if (settings.is_err()) {
      return base::make_err(diag::Reported{});
    }
    tables[filled++] = ProfileTable{.name = copy_str(arena, key.str()),
                                    .settings = std::move(settings).unwrap()};
  }
  const std::span<const ProfileTable> declared{tables, filled};
  return base::make_ok(Profiles{.tables = declared});
}

}  // namespace pkg
