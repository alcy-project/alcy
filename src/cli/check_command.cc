// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/check_command.h"

#include <string_view>
#include <utility>

#include "analyzer/resolve.h"
#include "base/logger.h"
#include "cli/cli_config.h"
#include "cli/result_code.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace cli {

namespace {

void log_check_result(const pipeline::CheckResult& result) {
  base::logger.wo_prefix("checked {} file(s), {} module(s), {} function(s)",
                         result.file_count, result.module_count,
                         result.function_count);
}

}  // namespace

ResultCode run_check(const CliConfig& config) {
  pipeline::PipelineContext ctx;
  const std::string_view raw_target =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<path::Path, path::PathError> target =
      path::Path::from_native(raw_target);
  if (target.is_err()) {
    base::logger.error("invalid target {}", raw_target);
    return ResultCode::CheckFailed;
  }
  const path::Path target_path = std::move(target).unwrap();

  const path::Path manifest_path = target_path.join(pkg::kManifestFileName);
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_ok()) {
    const pipeline::CheckResult result =
        pipeline::check_package(ctx, target_path, std::move(manifest).unwrap(),
                                manifest_path.as_view());
    ResultCode code = ResultCode::Success;
    if (result.success) {
      code = ResultCode::Success;
    } else {
      code = ResultCode::CheckFailed;
    }
    pipeline::report(ctx.bag, ctx.sources);
    log_check_result(result);
    return code;
  }

  // Single-file mode for explicit `foo.al` targets. Directories without
  // a manifest are not checked: module structure needs declared roots.
  if (raw_target.size() < 4 ||
      raw_target.substr(raw_target.size() - 3) != ".al") {
    base::logger.error(
        "no manifest found at '{}'; check a file or add alcy.toml", raw_target);
    return ResultCode::CheckFailed;
  }
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target_path.as_view());
  if (file.is_err()) {
    base::logger.error("cannot read '{}'", raw_target);
    return ResultCode::CheckFailed;
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      root, {&single_input, 1}, "", ctx.sources, ctx.ast, ctx.bag);
  if (tree.is_err()) {
    base::logger.error("resolve modules failed for '{}'", root);
    return ResultCode::CheckFailed;
  }
  if (ctx.bag.has_errors()) {
    pipeline::report(ctx.bag, ctx.sources);
    return ResultCode::CheckFailed;
  }

  pipeline::CheckResult result =
      pipeline::finish_check(ctx, std::move(tree).unwrap(), 1);
  ResultCode code = ResultCode::Success;
  if (result.success) {
    code = ResultCode::Success;
  } else {
    code = ResultCode::CheckFailed;
  }
  pipeline::report(ctx.bag, ctx.sources);

  log_check_result(result);
  return code;
}

}  // namespace cli
