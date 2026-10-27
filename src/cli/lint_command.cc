// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/lint_command.h"

#include <optional>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "linter/config.h"
#include "linter/lint.h"
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"

namespace cli {

namespace {

void finish(const linter::LintOutcome& result, Envelope& envelope) {
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Linted;
  envelope.file_count = result.file_count;
  envelope.module_count = result.module_count;
  envelope.lint_count = result.warning_count + result.error_count;
}

// The exit code a finished lint run takes: a denied finding is an error
// in the bag, and the verb fails when it has one. The command still
// filled the envelope, so the counts are reported either way.
ResultCode lint_result_code(const pipeline::PipelineContext& ctx) {
  if (ctx.bag.has_errors()) {
    return ResultCode::LintFailed;
  }
  return ResultCode::Success;
}

}  // namespace

ResultCode run_lint(const CliConfig& config,
                    pipeline::PipelineContext& ctx,
                    Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;

  // A single file has no manifest to read settings from, so the
  // configuration is the one beside the working directory - which is
  // where an editor starts the command.
  if (!config.file.empty()) {
    base::Result<path::Path, path::PathError> cwd =
        path::Path::from_native(".");
    if (cwd.is_err()) {
      return ResultCode::LintFailed;
    }
    base::Result<linter::LintConfig, diag::Reported> loaded =
        linter::load_config(ctx, std::move(cwd).unwrap());
    if (loaded.is_err()) {
      return ResultCode::LintFailed;
    }
    base::Result<linter::LintOutcome, diag::Reported> result =
        linter::lint_single_file(ctx, config.file, std::move(loaded).unwrap());
    envelope.trace = trace.take_events();
    if (result.is_err()) {
      return ResultCode::LintFailed;
    }
    finish(std::move(result).unwrap(), envelope);
    return lint_result_code(ctx);
  }

  const std::string_view raw_input = config.input.empty() ? "." : config.input;
  base::Result<pipeline::ManifestProbe, diag::Reported> probe =
      pipeline::require_package_manifest(ctx, raw_input, true);
  if (probe.is_err()) {
    return ResultCode::LintFailed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  base::Result<linter::LintConfig, diag::Reported> loaded =
      linter::load_config(ctx, found.root);
  if (loaded.is_err()) {
    return ResultCode::LintFailed;
  }
  linter::LintConfig lint_config = std::move(loaded).unwrap();
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(found.manifest);
  const bool suite =
      manifest_bytes.has_value() &&
      pkg::probe_manifest_kind(*manifest_bytes) == pkg::ManifestKind::Suite;
  base::Result<linter::LintOutcome, diag::Reported> result =
      suite ? linter::lint_suite(ctx, found.root, found.manifest,
                                 found.manifest_name, lint_config)
            : linter::lint_package(ctx, found.root, found.manifest,
                                   found.manifest_name, lint_config);
  envelope.trace = trace.take_events();
  if (result.is_err()) {
    return ResultCode::LintFailed;
  }
  finish(std::move(result).unwrap(), envelope);
  return lint_result_code(ctx);
}

}  // namespace cli
