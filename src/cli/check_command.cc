// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/check_command.h"

#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/target.h"

namespace cli {

namespace {

ResultCode finish(const pipeline::CheckResult& result, Envelope& envelope) {
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
    base::Result<pipeline::CheckResult, diag::Reported> result =
        pipeline::check_single_file(ctx, config.file);
    envelope.trace = trace.take_events();
    if (result.is_err()) {
      return failed;
    }
    return finish(std::move(result).unwrap(), envelope);
  }

  const bool check_current_dir = config.target_dir.empty();
  const std::string_view raw_target =
      check_current_dir ? "." : config.target_dir;
  base::Result<pipeline::ManifestProbe, path::PathError> probe =
      pipeline::find_package_manifest(ctx, raw_target);
  if (probe.is_err()) {
    return failed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();
  if (found.found) {
    base::Result<pipeline::CheckResult, diag::Reported> result =
        pipeline::check_package(ctx, found.root, found.manifest,
                                found.manifest_name);
    envelope.trace = trace.take_events();
    if (result.is_err()) {
      return failed;
    }
    return finish(std::move(result).unwrap(), envelope);
  }

  // Directories without a manifest are not checked: module structure
  // needs declared roots.
  if (check_current_dir) {
    const u32 index =
        ctx.bag
            .emit<i18n::Key::PipelineNoManifestWithFileHintInCurrentDirectory>(
                diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST);
    (void)index;
  } else {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineNoManifestWithFileHint>(
        diag::Severity::Error, pipeline::PIPELINE_NO_MANIFEST, raw_target);
    (void)index;
  }
  return failed;
}

}  // namespace cli
