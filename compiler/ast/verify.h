// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace ast {

// Structural failure of an AST arena: a child index names no node in
// its table. Invalid indices are allowed absent edges; anything else
// must be in range.
enum class VerifyError : u8 {
  DanglingType,
  DanglingPath,
  DanglingPattern,
  DanglingExpr,
  DanglingStmt,
  DanglingBlock,
  DanglingCond,
  DanglingItem,
  DanglingLiteral,
};

// Pure structural verification of an arena: every child index in
// every node is either invalid or names a node in its table.
// Parser-built arenas satisfy this by construction; hand-built
// arenas must pass before crossing into the analyzer. No I/O, no
// logging, no bag writes.
base::Result<void, VerifyError> verify_file(const AstArena& arena);

// Short human-readable detail for a verification failure.
std::string_view describe_verify_error(VerifyError error);

}  // namespace ast
