// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/build_command.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "app/driver_config.h"
#include "app/driver_context.h"
#include "app/result_code.h"
#include "app/runtime_stage.h"
#include "app/spawn.h"
#include "app/target.h"
#include "base/logger.h"
#include "borrow/borrow.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/llvm_object_emitter.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/pipeline.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace app {

namespace {

// Runs type checking, lowering, and borrow checking over a
// resolved tree. Reports nothing; callers report and map errors.
diag::Fallible<lower::LoweredPackage> compile_tree_to_ir(
    DriverContext& ctx,
    analyzer::ModuleTree tree) {
  diag::Fallible<analyzer::CheckedPackage> checked =
      analyzer::check_package(tree, kCheckWidth, ctx.bag);
  if (checked.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Fatal{});
  }
  diag::Fallible<lower::LoweredPackage> lowered = lower::lower_package(
      std::move(checked).unwrap(), kCheckWidth, ctx.strings, ctx.bag);
  if (lowered.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Fatal{});
  }
  lower::LoweredPackage package = std::move(lowered).unwrap();
  borrow::check_borrows(package, ctx.bag);
  if (ctx.bag.has_errors()) {
    return base::make_err(diag::Fatal{});
  }
  return base::make_ok(std::move(package));
}

// Emits one relocatable object for lowered IR.
bool emit_package_object(DriverContext& ctx,
                         lower::LoweredPackage& package,
                         bool optimize,
                         const std::string& output_path) {
  llvm::LLVMContext context;
  auto module = std::make_unique<llvm::Module>("alcy_module", context);
  codegen_llvm::LlvmIrEmitter emitter(module.get(), std::move(package.storage),
                                      &ctx.strings);
  std::move(emitter).emit();
  base::Result<void, codegen_llvm::ObjectEmitError> emitted =
      codegen_llvm::emit_object(*module, "", output_path, optimize);
  if (emitted.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "cannot emit object '{}'", output_path);
    (void)index;
    return false;
  }
  return true;
}

// Links one object plus the staged runtime into an executable
// through the system compiler driver.
bool link_executable(DriverContext& ctx,
                     const std::string& object_path,
                     const std::string& runtime_path,
                     const std::string& exe_path) {
  const char* system_linker = nullptr;
  if (const char* env = std::getenv("CC"); env && *env) {
    system_linker = env;
  } else {
    system_linker = "clang";
  }

  base::Result<i32, SpawnError> linked =
      run_command({system_linker, object_path, runtime_path, "-o", exe_path});
  if (linked.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverLinkError,
                                   "cannot run the system compiler");
    (void)index;
    return false;
  }
  const i32 code = std::move(linked).unwrap();
  if (code != 0) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverLinkError,
                                   "linking '{}' failed", exe_path);
    (void)index;
    return false;
  }
  return true;
}

// Single-file executable build (package builds stay on
// discovery until wires them). Runs the full frontend plus
// borrow checking, then lowers and emits one relocatable object.
ResultCode build_single_file(DriverContext& ctx,
                             std::string_view target,
                             std::string_view output,
                             bool optimize) {
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target);
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "cannot read '{}'", target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      root, {&single_input, 1}, "", ctx.sources, ctx.arena, ctx.bag);
  if (tree.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  diag::Fallible<lower::LoweredPackage> package =
      compile_tree_to_ir(ctx, std::move(tree).unwrap());
  if (package.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  lower::LoweredPackage lowered = std::move(package).unwrap();
  // An explicit .o output keeps object emission; otherwise the
  // single file links straight to an executable.
  std::string output_path =
      output.empty() ? std::string(target) : std::string(output);
  if (output.empty()) {
    // The target spells a source file, so an extension is present.
    const usize dot = output_path.rfind('.');
    output_path.replace(dot, std::string::npos, exe_suffix());
  }
  const bool want_object = output_path.size() >= 2 &&
                           output_path.substr(output_path.size() - 2) == ".o";
  if (want_object) {
    if (!emit_package_object(ctx, lowered, optimize, output_path)) {
      report(ctx.bag, ctx.sources);
      return ResultCode::BuildFailed;
    }
    report(ctx.bag, ctx.sources);
    base::logger.wo_prefix("built {} to {}", target, output_path);
    return ResultCode::Success;
  }
  io::TempDir scratch("alcy_build");
  const std::string object_path = scratch.join("main.o");
  if (!emit_package_object(ctx, lowered, optimize, object_path) ||
      !stage_runtime(scratch)) {
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  const std::string runtime_path = scratch.join(runtime_source_name());
  if (!link_executable(ctx, object_path, runtime_path, output_path)) {
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  report(ctx.bag, ctx.sources);
  base::logger.wo_prefix("built {} to {}", target, output_path);
  return ResultCode::Success;
}

ResultCode build_package(DriverContext& ctx,
                         const path::Path& root,
                         source::FileId manifest_file,
                         std::string_view manifest_name,
                         std::string_view output,
                         bool optimize) {
  diag::Fallible<pkg::PackageManifest> parsed =
      pkg::parse_manifest(ctx.sources.bytes(manifest_file), manifest_name,
                          manifest_file, ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return ResultCode::BuildFailed;
  }
  const pkg::PackageManifest manifest = std::move(parsed).unwrap();

  diag::Fallible<BinTarget> target =
      resolve_bin_target(ctx, root, manifest, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  BinTarget resolved = std::move(target).unwrap();
  diag::Fallible<lower::LoweredPackage> package =
      compile_tree_to_ir(ctx, resolved.tree);
  if (package.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  lower::LoweredPackage lowered = std::move(package).unwrap();
  std::string exe_path;
  if (output.empty()) {
    path::Path out_dir = root.join("out");
    // TODO: make directory helper in correct location
    // io::make_dirs(out_dir.c_str());
    exe_path =
        out_dir.join(std::string(resolved.bin_name) + std::string(exe_suffix()))
            .as_view();
  } else {
    exe_path = std::string(output);
  }
  io::TempDir scratch("alcy_build");
  const std::string object_path = scratch.join("main.o");
  if (!emit_package_object(ctx, lowered, optimize, object_path) ||
      !stage_runtime(scratch)) {
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  const std::string runtime_path = scratch.join(runtime_source_name());
  if (!link_executable(ctx, object_path, runtime_path, exe_path)) {
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  report(ctx.bag, ctx.sources);
  base::logger.wo_prefix("built {} to {}", manifest.name, exe_path);
  return ResultCode::Success;
}

}  // namespace

ResultCode run_build(const DriverConfig& config) {
  DriverContext ctx;

  const std::string_view raw_dir =
      config.target_dir.empty() ? "." : config.target_dir;
  if (raw_dir.size() >= kSourceSuffix.size() &&
      raw_dir.substr(raw_dir.size() - kSourceSuffix.size()) == kSourceSuffix) {
    return build_single_file(ctx, raw_dir, config.output, config.release);
  }
  base::Result<path::Path, path::PathError> dir =
      path::Path::from_native(raw_dir);
  if (dir.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "invalid target directory '{}'", raw_dir);
    (void)index;
    report(ctx.bag, ctx.sources);
    return ResultCode::BuildFailed;
  }
  const path::Path root = std::move(dir).unwrap();
  const path::Path manifest_path = root.join(pkg::kManifestFileName);

  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_ok()) {
    return build_package(ctx, root, std::move(manifest).unwrap(),
                         manifest_path.as_view(), config.output,
                         config.release);
  }

  const u32 index = ctx.bag.emit(
      diag::Severity::Warning, kDriverNoManifest,
      "no manifest found at '{}'; building directory directly", raw_dir);
  (void)index;
  diag::Fallible<pipeline::DiscoveredSources> discovered =
      pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);
  report(ctx.bag, ctx.sources);
  if (discovered.is_err() || ctx.bag.has_errors()) {
    return ResultCode::BuildFailed;
  }
  base::logger.wo_prefix("built {} file(s)",
                         std::move(discovered).unwrap().files.size());
  return ResultCode::Success;
}

}  // namespace app

