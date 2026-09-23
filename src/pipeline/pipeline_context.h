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

namespace pipeline {

// Diagnostic codes 3000-3099 are reserved for the pipeline.
inline constexpr u32 kPipelineNoManifest = 3000;
inline constexpr u32 kPipelineIoError = 3001;
inline constexpr u32 kPipelineNotImplemented = 3002;
inline constexpr u32 kPipelineNoTargets = 3003;
inline constexpr u32 kPipelineLinkError = 3004;

struct PipelineContext {
  mem::Arena arena;
  ast::AstArena ast;
  source::SourceManager sources;
  diag::DiagBag bag;
  // Long-lived string pool for lowering and codegen (function names,
  // string literals). Must outlive every phase that reads its ids.
  str::StringInterner strings;

  PipelineContext();
};

// Returns ".exe" on Windows or else ""
std::string_view exe_suffix();

// Renders every diagnostic in the bag through the logger.
void report(const diag::DiagBag& bag, const source::SourceManager& sources);

}  // namespace pipeline
