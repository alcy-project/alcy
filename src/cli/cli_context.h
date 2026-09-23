// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "fpag/str/string_interner.h"
#include "source/source.h"

namespace cli {

// Diagnostic codes 3000-3099 are reserved for the cli.
inline constexpr u32 kCliNoManifest = 3000;
inline constexpr u32 kCliIoError = 3001;
inline constexpr u32 kCliNotImplemented = 3002;
inline constexpr u32 kCliNoTargets = 3003;
inline constexpr u32 kCliLinkError = 3004;

constexpr std::string_view kSourceSuffix = ".al";

struct CliContext {
  mem::Arena arena;
  ast::AstArena ast;
  source::SourceManager sources;
  diag::DiagBag bag;
  // Long-lived string pool for lowering and codegen (function names,
  // string literals). Must outlive every phase that reads its ids.
  str::StringInterner strings;

  CliContext();
};

// Returns ".exe" on Windows or else ""
std::string_view exe_suffix();

// Renders every diagnostic in the bag through the logger.
void report(const diag::DiagBag& bag, const source::SourceManager& sources);

}  // namespace cli
