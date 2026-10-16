// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/build_command.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cli/cli_config.h"
#include "cli/output.h"
#include "cli/result_code.h"
#include "cli/trace.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "pipeline/build.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/profile.h"
#include "pipeline/target.h"
#include "pkg/manifest.h"
#include "pkg/profile.h"
#include "pkg/toolchain.h"

namespace cli {

ResultCode run_build(const CliConfig& config,
                     pipeline::PipelineContext& ctx,
                     Envelope& envelope) {
  TraceSession trace(ctx, config.time_trace);
  envelope.bag = &ctx.bag;
  envelope.sources = &ctx.sources;
  const ResultCode failed = ResultCode::BuildFailed;
  // Validation keeps single files out: `build` takes a package
  // directory, and `compile` takes the file.
  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<pipeline::ManifestProbe, diag::Reported> probe =
      pipeline::require_package_manifest(ctx, raw_dir, false);
  if (probe.is_err()) {
    return failed;
  }
  pipeline::ManifestProbe found = std::move(probe).unwrap();

  base::Result<pkg::Toolchain, diag::Reported> toolchain =
      pipeline::load_toolchain(ctx, found.root);
  if (toolchain.is_err()) {
    return failed;
  }
  const pkg::Toolchain tool = std::move(toolchain).unwrap();
  ctx.freestanding = tool.freestanding;
  ctx.no_cache = config.no_cache;
  const pipeline::LinkOptions link = resolve_link_options(config, tool);
  base::Result<pkg::Profiles, diag::Reported> profiles =
      pipeline::load_profiles(ctx, found.root);
  if (profiles.is_err()) {
    return failed;
  }
  const pkg::Profiles tables = std::move(profiles).unwrap();
  base::Result<pipeline::BuildProfile, diag::Reported> profile =
      pipeline::resolve_profile(
          pipeline::ProfileRequest{.name = config.profile,
                                   .on_panic = config.on_panic,
                                   .optimize = config.optimize,
                                   .overflow_checks = config.overflow_checks},
          tables, ctx.bag);
  if (profile.is_err()) {
    return failed;
  }
  const pipeline::BuildProfile selected = std::move(profile).unwrap();
  resolve_backend_options(config, ctx, selected);
  // A suite directory builds every member; a package directory builds
  // itself (ADR-0057).
  const std::optional<std::string_view> manifest_bytes =
      ctx.sources.bytes(found.manifest);
  const bool suite =
      manifest_bytes.has_value() &&
      pkg::probe_manifest_kind(*manifest_bytes) == pkg::ManifestKind::Suite;
  base::Result<std::string, diag::Reported> res =
      suite ? pipeline::build_suite(ctx, found.root, found.manifest,
                                    found.manifest_name, config.output,
                                    selected, link, config.emit)
            : pipeline::build_package(ctx, found.root, found.manifest,
                                      found.manifest_name, config.output,
                                      selected, link, config.emit);
  envelope.trace = trace.take_events();
  if (res.is_err() || ctx.bag.has_errors()) {
    return failed;
  }
  record_output(std::move(res).unwrap(), envelope);
  envelope.status = Status::Ok;
  envelope.outcome = Outcome::Built;
  envelope.cache = ctx.cache_outcome;
  return ResultCode::Success;
}

}  // namespace cli
