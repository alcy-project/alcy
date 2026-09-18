// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <span>

#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/mem/arena.h"

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
void desugar_shadowing(std::span<ast::Item* const> items,
                       mem::Arena& arena,
                       diag::DiagBag& bag);

}  // namespace parser
