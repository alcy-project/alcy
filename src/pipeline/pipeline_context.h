// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profiler.h"
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
  // Standard library sources, populated by std_prelude and kept alive
  // for the command: the names borrow the generated tables and the bytes
  // live in the source manager under those names. Restaged when a later
  // call selects different members.
  bool std_staged = false;
  std::vector<std::string_view> std_selected;
  std::vector<analyzer::ModuleInput> std_inputs;
  // Profiler the phase scopes record into; null records nothing. The cli
  // points it at Profiler::global() when --time-trace is given; a host
  // embedding the pipeline points it at its own instance.
  debug::Profiler* profiler = nullptr;

  PipelineContext();
};

// Returns ".exe" on Windows or else ""
std::string_view exe_suffix();

// Creates every missing directory leading to `path`, and `path` itself
// when it names a directory rather than a file. A path that fails
// validation is left to the caller's own write to report.
base::Result<void, diag::Reported> ensure_directories(PipelineContext& ctx,
                                                      std::string_view path);

}  // namespace pipeline
