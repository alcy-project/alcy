// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/toolchain.h"

#include <string_view>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "pkg/arena_copy.h"
#include "source/source.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#pragma clang diagnostic ignored "-Wswitch"
// clang-format off
// Umbrella header provides the .inl implementations; keep it whole.
#include "toml++/toml.hpp"  // IWYU pragma: keep
// Other headers must be included after toml.hpp
#include "toml++/impl/node.hpp"
#include "toml++/impl/parse_error.hpp"
#include "toml++/impl/parse_result.hpp"
#include "toml++/impl/parser.hpp"
#include "toml++/impl/table.hpp"

#pragma clang diagnostic pop

namespace pkg {

namespace {

// Diagnostic codes 1300-1399 are reserved for toolchain errors.
constexpr u32 TOOLCHAIN_SYNTAX_ERROR = 1300;
constexpr u32 TOOLCHAIN_SEMANTIC_ERROR = 1301;

}  // namespace

base::Result<Toolchain, diag::Reported> parse_toolchain(
    std::string_view bytes,
    std::string_view filename,
    source::FileId /*file*/,
    diag::DiagBag& bag,
    mem::Arena& arena) {
  toml::parse_result result = toml::parse(bytes, filename);
  if (!result) {
    // The file holds one key, so the message names the file rather
    // than pointing into it.
    const toml::parse_error& error = result.error();
    const u32 index = bag.emit(diag::Severity::Error, TOOLCHAIN_SYNTAX_ERROR,
                               "toolchain '{}': TOML syntax error: {}",
                               filename, error.description());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const toml::table& root = result.table();
  std::string_view linker;
  for (const auto& [key, node] : root) {
    if (key.str() != "linker") {
      continue;
    }
    const auto text = node.value<std::string_view>();
    if (!text.has_value()) {
      const u32 index = bag.emit(diag::Severity::Error,
                                 TOOLCHAIN_SEMANTIC_ERROR,
                                 "toolchain '{}': linker must be a string",
                                 filename);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    linker = copy_str(arena, *text);
  }
  return base::make_ok(Toolchain{.linker = linker});
}

}  // namespace pkg
