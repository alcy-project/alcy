// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/check_command.h"

#include <optional>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/target.h"
#include "pkg/manifest.h"

namespace cli {

namespace {

ResultCode finish(const pipeline::CheckOutcome& result, Envelope& envelope) {
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Checked;
  envelope.file_count = result.file_count;
  envelope.module_count = result.module_count;
  envelope.function_count = result.function_count;
  return ResultCode::Success;
}

}  // namespace

ResultCode run_check(const CliConfig& config,
                     pipeline::PipelineContext& ctx,
                     Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const ResultCode failed = ResultCode::CheckFailed;

  // Validation keeps the two forms apart: `--file` names one source,
  // and the positional names a package directory.
  if (!config.file.empty()) {
    base::Result<pipeline::CheckOutcome, diag::Reported> result =
        pipeline::check_single_file(ctx, config.file);
    envelope.trace = trace.take_events();
    if (result.is_err()) {
      return failed;
    }
    return finish(std::move(result).unwrap(), envelope);
  }

  const std::string_view raw_target =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<pipeline::ManifestProbe, diag::Reported> probe =
      pipeline::require_package_manifest(ctx, raw_target, true);
  if (probe.is_err()) {
    return failed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(found.manifest);
  const bool suite =
      manifest_bytes.has_value() &&
      pkg::probe_manifest_kind(*manifest_bytes) == pkg::ManifestKind::Suite;
  base::Result<pipeline::CheckOutcome, diag::Reported> result =
      suite ? pipeline::check_suite(ctx, found.root, found.manifest,
                                    found.manifest_name)
            : pipeline::check_package(ctx, found.root, found.manifest,
                                      found.manifest_name);
  envelope.trace = trace.take_events();
  if (result.is_err()) {
    return failed;
  }
  return finish(std::move(result).unwrap(), envelope);
}

}  // namespace cli
