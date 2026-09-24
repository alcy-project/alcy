// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/run.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/runtime_stage.h"
#include "pipeline/spawn.h"
#include "pipeline/std_stage.h"
#include "pipeline/target.h"
#include "source/source.h"

namespace pipeline {

namespace {

// Links lowered IR in a scratch directory and executes it with
// inherited stdio, forwarding args to the program.
RunResult link_and_run(PipelineContext& ctx,
                       lower::LoweredPackage& lowered,
                       bool optimize,
                       std::string_view linker,
                       std::span<const std::string_view> args) {
  io::TempDir scratch("alcy_run");
  const std::string object_path = scratch.join("main.o");
  if (!emit_package_object(ctx, lowered, optimize, object_path) ||
      !stage_runtime(scratch)) {
    return RunResult{false, 0};
  }
  const std::string exe_path =
      scratch.join(std::string("main") + std::string(exe_suffix()));
  const std::string runtime_path = scratch.join(runtime_source_name());
  if (!link_executable(ctx, linker, object_path, runtime_path, exe_path)) {
    return RunResult{false, 0};
  }
  std::vector<std::string> argv;
  argv.reserve(args.size() + 1);
  argv.emplace_back(exe_path);
  for (std::string_view arg : args) {
    argv.emplace_back(arg);
  }
  base::Result<i32, SpawnError> executed = run_command(argv);
  if (executed.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineLinkError,
                                   "cannot execute '{}'", exe_path);
    (void)index;
    return RunResult{false, 0};
  }
  return RunResult{true, std::move(executed).unwrap()};
}

}  // namespace

RunResult run_single_file(PipelineContext& ctx,
                          std::string_view target,
                          bool optimize,
                          std::string_view linker,
                          std::span<const std::string_view> args) {
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target);
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot read '{}'", target);
    (void)index;
    return RunResult{false, 0};
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  diag::Fallible<analyzer::ModuleTree> tree =
      analyzer::resolve_modules(root, {&single_input, 1}, "", ctx.sources,
                                ctx.ast, ctx.bag, std_prelude(ctx));
  if (tree.is_err() || ctx.bag.has_errors()) {
    return RunResult{false, 0};
  }
  diag::Fallible<lower::LoweredPackage> package =
      compile_tree(ctx, std::move(tree).unwrap());
  if (package.is_err() || ctx.bag.has_errors()) {
    return RunResult{false, 0};
  }
  lower::LoweredPackage lowered = std::move(package).unwrap();
  return link_and_run(ctx, lowered, optimize, linker, args);
}

RunResult run_package(PipelineContext& ctx,
                      const path::Path& root,
                      source::FileId manifest_file,
                      std::string_view manifest_name,
                      bool optimize,
                      std::string_view linker,
                      std::span<const std::string_view> args) {
  diag::Fallible<BinTarget> target =
      resolve_package_target(ctx, root, manifest_file, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    return RunResult{false, 0};
  }
  BinTarget resolved = std::move(target).unwrap();
  diag::Fallible<lower::LoweredPackage> package =
      compile_tree(ctx, resolved.tree);
  if (package.is_err() || ctx.bag.has_errors()) {
    return RunResult{false, 0};
  }
  lower::LoweredPackage lowered = std::move(package).unwrap();
  return link_and_run(ctx, lowered, optimize, linker, args);
}

}  // namespace pipeline
