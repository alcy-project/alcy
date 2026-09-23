// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

#include "ast/ast.h"
#include "diag/bag.h"

namespace parser {

// Alpha-renames shadowed bindings so every use refers to a lexically
// unique spelling: the first declaration keeps its name, later ones in
// nested scopes become `name$n`. Single-segment value paths resolve
// through the same scopes. Module paths, type paths, field and method
// names are never touched. Keyword spellings (`self`, `super`,
// `package`, `Self`) map to themselves.
//
// One uniform rule covers every case: declaring a name already present
// in the innermost scope is an error (same-pattern duplicates,
// same-block redeclarations, duplicate parameters). Shadowing across
// nested scopes freshens. `or`-pattern alternatives share one scope
// each, pre-seeded from the first alternative, and must bind identical
// name sets.
void desugar_shadowing(std::span<const ast::ItemIdx> items,
                       ast::AstArena& ast,
                       diag::DiagBag& bag);

}  // namespace parser
