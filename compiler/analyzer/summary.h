// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "ir/deserialize.h"
#include "ir/symbol_table.h"

namespace analyzer {

// A summarized library spelled into the syntax arena: the modules its
// surface names and the items each holds. The items carry no bodies, so
// nothing in the package is lowered, and the checker registers them the
// way it registers a source dependency (ADR-0078). The caller keeps
// `summary` alive for as long as the tree uses the names it spells.
struct SummarySurface {
  // The identity a `use` spells for the package.
  std::string identity;
  struct Module {
    // The module path, dotted from the package root; empty for the root
    // itself.
    std::string path;
    // The module's items, and everything they point at, live in the
    // arena the surface was spelled into.
    std::span<const ast::ItemIdx> items;
    // The symbol each item links by, parallel to `items`.
    std::span<const SummarySymbol> symbols;
  };
  std::vector<Module> modules;
};

// The name a `use` writes for a summary module: a dotted path with
// slash separators, its runs collapsed.
[[nodiscard]] std::string summary_module_name(std::string_view dotted);

// Spells `summary`'s surface into `ast`, copying every name it spells
// into `strings` so nothing borrows the artifact. Free functions whose
// signatures the syntax can carry are spelled whole; a row over a
// nominal, an associated function or method, and every generic
// declaration is left out until the summary carries what they need. An
// exhausted arena stops the walk, and the caller's `ast.exhausted()`
// check answers for it.
[[nodiscard]] SummarySurface spell_summary_surface(const ir::LoadedIr& summary,
                                                   std::string_view identity,
                                                   ast::AstArena& ast,
                                                   ir::SymbolTable& strings);

}  // namespace analyzer
