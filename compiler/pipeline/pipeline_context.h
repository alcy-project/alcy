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
#include "i18n/language.h"
#include "source/source.h"

namespace pipeline {

// Diagnostic codes 8000-8099 are reserved for the pipeline.

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
  // How many threads the parse stage may read files with. Zero means the
  // caller did not ask for any.
  u32 jobs = 0;

  // `span_capacity` is the syntax arena's reservation. A case that has
  // to spend it can ask for less, the way `ast::AstArena` allows.
  explicit PipelineContext(
      i18n::Language language,
      usize span_capacity = ast::AstArena::DEFAULT_SPAN_CAPACITY);

  // The thread count `parse_files` reads with, which is one unless the
  // command line asked for more. Reading files is the only work spread
  // over threads, so nothing after it is bounded by this.
  [[nodiscard]] u32 parse_jobs() const;
};

// Returns ".exe" on Windows or else ""
std::string_view exe_suffix();

// Creates every missing directory leading to `path`, and `path` itself
// when it names a directory rather than a file. A path that fails
// validation is left to the caller's own write to report.
base::Result<void, diag::Reported> ensure_directories(PipelineContext& ctx,
                                                      std::string_view path);

}  // namespace pipeline
