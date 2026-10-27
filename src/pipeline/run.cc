// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/run.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
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
#include "pipeline/pipeline_context.h"
#include "pipeline/spawn.h"
#include "source/source.h"

namespace pipeline {

namespace {

// Links lowered IR in a scratch directory and executes it with
// inherited stdio, forwarding args to the program.
base::Result<RunOutcome, diag::Reported> link_and_run(
    PipelineContext& ctx,
    lowering::LoweredPackage& lowered,
    std::string_view target,
    BuildPlan plan,
    std::span<const std::string> linked_objects,
    std::span<const std::string_view> args,
    AnnounceExec announce,
    const void* announce_ctx) {
  io::TempDir scratch = io::TempDir::create_unique("alcy_run_");
  base::Result<std::vector<std::string>, diag::Reported> objects =
      emit_package_objects(ctx, lowered, plan, scratch, false,
                           codegen_parts(lowered));
  if (objects.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const std::string exe_path =
      scratch.join(std::string("main") + std::string(exe_suffix()));
  std::vector<std::string> paths = std::move(objects).unwrap();
  // The dependencies a summary answered for bring their own objects,
  // as they do for a build.
  paths.insert(paths.end(), linked_objects.begin(), linked_objects.end());
  if (link_executable(ctx, plan.link, paths, exe_path).is_err()) {
    return base::make_err(diag::Reported{});
  }
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
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::LinkError,
        exe_path);
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
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets,
        manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  Artifact& bin = resolved.front();
  if (!check_entry_point(ctx, bin)) {
    return base::make_err(diag::Reported{});
  }
  base::Result<lowering::LoweredPackage, diag::Reported> package =
      compile_tree(ctx, bin.tree, plan, true, bin.name);
  if (package.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lowering::LoweredPackage lowered = std::move(package).unwrap();
  return link_and_run(ctx, lowered, bin.name, plan, bin.linked_objects, args,
                      announce, announce_ctx);
}

}  // namespace pipeline
