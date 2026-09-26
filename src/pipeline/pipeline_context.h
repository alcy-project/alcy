// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "fpag/str/string_interner.h"
#include "source/source.h"

namespace pipeline {

// Diagnostic codes 8000-8099 are reserved for the pipeline.
inline constexpr u32 PIPELINE_NO_MANIFEST = 8000;
inline constexpr u32 PIPELINE_IO_ERROR = 8001;
inline constexpr u32 PIPELINE_NOT_IMPLEMENTED = 8002;
inline constexpr u32 PIPELINE_NO_TARGETS = 8003;
inline constexpr u32 PIPELINE_LINK_ERROR = 8004;

struct PipelineContext {
  mem::Arena arena;
  ast::AstArena ast;
  source::SourceManager sources;
  diag::DiagBag bag;
  // Long-lived string pool for lowering and codegen (function names,
  // string literals). Must outlive every phase that reads its ids.
  str::StringInterner strings;
  // Staged standard library, populated once by std_prelude and kept
  // alive for the command. Inputs borrow the scratch paths.
  bool std_staged = false;
  std::optional<io::TempDir> std_scratch;
  std::vector<analyzer::ModuleInput> std_inputs;

  PipelineContext();
};

// Returns ".exe" on Windows or else ""
std::string_view exe_suffix();

}  // namespace pipeline
