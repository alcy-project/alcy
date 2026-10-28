// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/run.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "i18n/messages.h"
#include "lowering/lowering.h"
#include "path/path.h"
#include "pipeline/artifact.h"
#include "pipeline/build.h"
#include "pipeline/build_plan.h"
#include "pipeline/diag_code.h"
#include "pipeline/emit_mode.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/spawn.h"
#include "source/source.h"

namespace pipeline {

namespace {

// Executes a linked program with inherited stdio, forwarding args to it.
base::Result<RunOutcome, diag::Reported> execute(
    PipelineContext& ctx,
    const std::string& exe_path,
    std::string_view target,
    std::span<const std::string_view> args,
    AnnounceExec announce,
    const void* announce_ctx) {
  std::vector<std::string> argv;
  argv.reserve(args.size() + 1);
  argv.emplace_back(exe_path);
  for (std::string_view arg : args) {
    argv.emplace_back(arg);
  }
  // Announced here rather than at the entry point because everything
  // above it is the compiler's own work: until the program starts, the
  // reader is waiting on a build, and saying otherwise would be a
  // claim about work that has not been done.
  if (announce != nullptr) {
    announce(target, announce_ctx);
  }
  base::Result<i32, SpawnError> executed = run_command(argv);
  if (executed.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotExecute>(
        DiagCode::LinkError, exe_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(RunOutcome{.exit_code = std::move(executed).unwrap()});
}

}  // namespace

base::Result<RunOutcome, diag::Reported> run_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    BuildPlan plan,
    std::span<const std::string_view> args,
    AnnounceExec announce,
    const void* announce_ctx) {
  base::Result<std::vector<Artifact>, diag::Reported> targets =
      resolve_artifacts(ctx, root, manifest_file, manifest_name,
                        ArtifactScope::BinOnly);
  if (targets.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  std::vector<Artifact> resolved = std::move(targets).unwrap();
  if (resolved.empty()) {
    // A library builds but never runs: there is no entry to execute.
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNoBinToRun>(
        DiagCode::NoTargets, manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  Artifact& bin = resolved.front();
  if (!check_entry_point(ctx, bin)) {
    return base::make_err(diag::Reported{});
  }
  // The executable lands in a scratch directory the run owns: a run
  // leaves no artifact of its own behind - the cache entry the lowering
  // stores is the build's shape, keyed the same way - and the front half
  // of the lifecycle is the
  // one a build runs - cache included, so a run after a build rebuilds
  // nothing.
  io::TempDir scratch = io::TempDir::create_unique("alcy_run_");
  const std::string exe_path =
      scratch.join(std::string("main") + std::string(exe_suffix()));
  const PlannedArtifact planned{&bin, EmitMode::Executable, exe_path};
  base::Result<lowering::LoweredPackage, diag::Reported> lowered =
      lower_target(ctx, planned, root, manifest_file);
  if (lowered.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lowering::LoweredPackage package = std::move(lowered).unwrap();
  if (emit_target(ctx, planned, plan, package).is_err() ||
      ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  return execute(ctx, exe_path, bin.name, args, announce, announce_ctx);
}

}  // namespace pipeline
