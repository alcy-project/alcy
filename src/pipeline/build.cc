// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/build.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "borrow/borrow.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/llvm_object_emitter.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/runtime_stage.h"
#include "pipeline/spawn.h"
#include "pipeline/target.h"
#include "source/source.h"

namespace pipeline {

// Runs type checking, lowering, and borrow checking over a
// resolved tree.
diag::Fallible<lower::LoweredPackage> compile_tree(PipelineContext& ctx,
                                                   analyzer::ModuleTree tree) {
  diag::Fallible<analyzer::CheckedPackage> checked =
      analyzer::check_package(tree, kTargetWidth, ctx.ast, ctx.bag);
  if (checked.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Fatal{});
  }
  diag::Fallible<lower::LoweredPackage> lowered = lower::lower_package(
      std::move(checked).unwrap(), kTargetWidth, ctx.ast, ctx.strings, ctx.bag);
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
bool emit_package_object(PipelineContext& ctx,
                         lower::LoweredPackage& package,
                         bool optimize,
                         const std::string& output_path) {
  llvm::LLVMContext context;
  // TODO: Add optimization level enum
  (void)optimize;
  auto module = std::make_unique<llvm::Module>("alcy_module", context);
  codegen_llvm::LlvmIrEmitter emitter(module.get(), std::move(package.storage),
                                      &ctx.strings);
  std::move(emitter).emit();
  base::Result<std::vector<u8>, codegen_llvm::ObjectEmitError> emitted =
      codegen_llvm::emit_object(*module, "");
  if (emitted.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot emit object '{}'", output_path);
    (void)index;
    return false;
  }
  const std::vector<u8>& buffer = std::move(emitted).unwrap();
  io::write_file(buffer, output_path);
  return true;
}

// Links one object plus the staged runtime into an executable
// through the system linker. Empty selects the default toolchain driver.
bool link_executable(PipelineContext& ctx,
                     std::string_view linker,
                     const std::string& object_path,
                     const std::string& runtime_path,
                     const std::string& exe_path) {
  const std::string driver = linker.empty() ? "clang" : std::string(linker);

  base::Result<i32, SpawnError> linked =
      run_command({driver, object_path, runtime_path, "-o", exe_path});
  if (linked.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineLinkError,
                                   "cannot run the system compiler");
    (void)index;
    return false;
  }
  const i32 code = std::move(linked).unwrap();
  if (code != 0) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineLinkError,
                                   "linking '{}' failed", exe_path);
    (void)index;
    return false;
  }
  return true;
}

// Single-file executable build (package builds stay on
// discovery until wires them). Runs the full frontend plus
// borrow checking, then lowers and emits one relocatable object.
BuildResult build_single_file(PipelineContext& ctx,
                              std::string_view target,
                              std::string_view output,
                              bool optimize,
                              std::string_view linker) {
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target);
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot read '{}'", target);
    (void)index;
    return base::make_err(0);
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      root, {&single_input, 1}, "", ctx.sources, ctx.ast, ctx.bag);
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(0);
  }
  diag::Fallible<lower::LoweredPackage> package =
      compile_tree(ctx, std::move(tree).unwrap());
  if (package.is_err() || ctx.bag.has_errors()) {
    return base::make_err(0);
  }
  lower::LoweredPackage lowered = std::move(package).unwrap();
  // An explicit .o output keeps object emission; otherwise the
  // single file links straight to an executable.
  std::string output_path =
      output.empty() ? std::string(target) : std::string(output);
  if (output.empty()) {
    // The target spells a source file, so an extension is present.
    const usize dot = output_path.rfind('.');
    DCHECK(dot != std::string::npos);
    output_path.replace(dot, std::string::npos, exe_suffix());
  }
  const bool want_object = output_path.size() >= 2 &&
                           output_path.substr(output_path.size() - 2) == ".o";
  if (want_object) {
    if (!emit_package_object(ctx, lowered, optimize, output_path)) {
      return base::make_err(0);
    }
    return base::make_ok();
  }
  io::TempDir scratch("alcy_build");
  const std::string object_path = scratch.join("main.o");
  if (!emit_package_object(ctx, lowered, optimize, object_path) ||
      !stage_runtime(scratch)) {
    return base::make_err(0);
  }
  const std::string runtime_path = scratch.join(runtime_source_name());
  if (!link_executable(ctx, linker, object_path, runtime_path, output_path)) {
    return base::make_err(0);
  }
  return base::make_ok();
}

BuildResult build_package(PipelineContext& ctx,
                          const path::Path& root,
                          source::FileId manifest_file,
                          std::string_view manifest_name,
                          std::string_view output,
                          bool optimize,
                          std::string_view linker) {
  diag::Fallible<BinTarget> target =
      resolve_package_target(ctx, root, manifest_file, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    return base::make_err(0);
  }
  BinTarget resolved = std::move(target).unwrap();
  diag::Fallible<lower::LoweredPackage> package =
      compile_tree(ctx, resolved.tree);
  if (package.is_err() || ctx.bag.has_errors()) {
    return base::make_err(0);
  }
  lower::LoweredPackage lowered = std::move(package).unwrap();
  std::string exe_path;
  if (output.empty()) {
    path::Path out_dir = root.join("out");
    io::create_directory(out_dir.c_str());
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
    return base::make_err(0);
  }
  const std::string runtime_path = scratch.join(runtime_source_name());
  if (!link_executable(ctx, linker, object_path, runtime_path, exe_path)) {
    return base::make_err(0);
  }
  return base::make_ok();
}

}  // namespace pipeline

