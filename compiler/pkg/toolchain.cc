// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/toolchain.h"

#include <string_view>

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

namespace {}  // namespace

base::Result<Toolchain, diag::Reported> parse_toolchain(
    std::string_view bytes,
    std::string_view filename,
    source::FileId /*file*/,
    diag::DiagBag& bag,
    mem::Arena& arena) {
  toml::parse_result result = toml::parse(bytes, filename);
  if (!result) {
    // The toolchain file is read without a source location, so the
    // message names the file rather than pointing into it.
    const toml::parse_error& error = result.error();
    const u32 index = bag.emit<i18n::Key::PkgToolchainTomlSyntaxError>(
        diag::Severity::Error, diag::Stage::Pkg, DiagCode::ToolchainSyntaxError,
        filename, error.description());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const toml::table& root = result.table();
  std::string_view linker;
  const toml::array* link_args = nullptr;
  for (const auto& [key, node] : root) {
    const std::string_view name = key.str();
    if (name == "linker") {
      const auto text = node.value<std::string_view>();
      if (!text.has_value()) {
        const u32 index = bag.emit<i18n::Key::PkgToolchainLinkerNotAString>(
            diag::Severity::Error, diag::Stage::Pkg,
            DiagCode::ToolchainSemanticError, filename);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      linker = copy_str(arena, *text);
      continue;
    }
    if (name == "link-args") {
      if (!node.is_array()) {
        const u32 index = bag.emit<i18n::Key::PkgToolchainLinkArgsNotStrings>(
            diag::Severity::Error, diag::Stage::Pkg,
            DiagCode::ToolchainSemanticError, filename);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      link_args = node.as_array();
    }
  }

  // One block in the arena, like every other string this returns, so the
  // result outlives the parse's own buffers.
  const usize count = link_args == nullptr ? 0 : link_args->size();
  std::string_view* args = nullptr;
  if (count > 0) {
    args = static_cast<std::string_view*>(arena.alloc(
        sizeof(std::string_view) * count, alignof(std::string_view)));
  }
  u32 filled = 0;
  if (link_args != nullptr) {
    for (const toml::node& entry : *link_args) {
      const auto text = entry.value<std::string_view>();
      if (!text.has_value()) {
        const u32 index = bag.emit<i18n::Key::PkgToolchainLinkArgsNotStrings>(
            diag::Severity::Error, diag::Stage::Pkg,
            DiagCode::ToolchainSemanticError, filename);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      args[filled++] = copy_str(arena, *text);
    }
  }
  return base::make_ok(
      Toolchain{.linker = linker, .link_args = {args, filled}});
}

}  // namespace pkg
