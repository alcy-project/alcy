// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "linter/config.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace linter {

// What one lint run covered and found. The counts are the linter own;
// the invocation exit code reads the bag, where a denied finding is an
// error, so a run that emitted only warnings still succeeds.
struct LintOutcome {
  usize file_count = 0;
  usize module_count = 0;
  usize warning_count = 0;
  usize error_count = 0;
};

// Reads the goal package .alcy/lint.toml. An absent file is the
// defaults, not an error; a file that does not parse is. The settings
// are copied into the context arena, so they outlive the bytes.
base::Result<LintConfig, diag::Reported> load_config(
    pipeline::PipelineContext& ctx,
    const path::Path& package_root);

// Lints one source file, named on the command line. The standard library
// is staged the way a single-file check stages it.
base::Result<LintOutcome, diag::Reported> lint_single_file(
    pipeline::PipelineContext& ctx,
    std::string_view target,
    const LintConfig& config);

// Lints a single source held in memory. `name` is what diagnostics and
// module naming see and need not be a path, so a case with text but no
// file lints without touching the filesystem.
base::Result<LintOutcome, diag::Reported> lint_source(
    pipeline::PipelineContext& ctx,
    std::string_view name,
    std::string_view bytes,
    const LintConfig& config);

// Lints every target of the package rooted at root, after the analysis
// that build and check share. A package that does not analyze is not
// linted: every finding would be about code the compiler already
// refused, and the analysis errors are the useful answer.
base::Result<LintOutcome, diag::Reported> lint_package(
    pipeline::PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    const LintConfig& config);

// Lints every member of the suite at `root`, each through its own
// manifest. One member that cannot be linted fails the run after the
// others report, the way check_suite does.
base::Result<LintOutcome, diag::Reported> lint_suite(
    pipeline::PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    const LintConfig& config);

}  // namespace linter
