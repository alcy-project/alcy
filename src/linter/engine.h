// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "fpag/base/numeric.h"
#include "linter/config.h"
#include "pipeline/pipeline_context.h"

namespace linter {

// What one lint run reported, split by what the levels mean: a warning
// does not fail the run and a denied finding does. The counts cover
// only findings this run emitted, so a bag that already held analyzer
// errors is not counted against the linter.
struct LintStats {
  usize warnings = 0;
  usize errors = 0;

  [[nodiscard]] usize findings() const { return warnings + errors; }
};

// Runs every enabled rule over the goal package's own modules. Staged
// toolchain sources and path dependencies are skipped: a lint is about
// the code the reader wrote, and a dependency's warnings are its own.
LintStats run_lints(pipeline::PipelineContext& ctx,
                    const analyzer::CheckedPackage& package,
                    const LintConfig& config);

// Whether the module is the goal package's own code, which is what
// makes a module lintable. The engine skips the rest; a rule whose
// subject reaches past that boundary (the dead-code rule reads the call
// graph across every module) asks this rather than rebuilding it.
bool lintable(const analyzer::ModuleTree& tree, u32 module);

}  // namespace linter
