// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/build.h"

#include <memory>
#include <span>
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
#include "pipeline/std_stage.h"
#include "pipeline/target.h"
#include "source/source.h"

namespace pipeline {

namespace {

// Creates every missing directory leading to `output_path` so object
// emission never fails on a missing output directory. Bare file names
// need nothing; paths that fail validation skip creation and let the
// subsequent write report the real failure.
base::Result<void, diag::Reported> ensure_parent_directories(
    PipelineContext& ctx,
    const std::string& output_path) {
  base::Result<path::Path, path::PathError> parsed =
      path::Path::from_native(output_path);
  if (parsed.is_err()) {
    return base::make_ok();
  }
  const path::Path parent = std::move(parsed).unwrap().parent();
  const std::string_view dir = parent.as_view();
  if (dir == "." || dir == "/") {
    return base::make_ok();
  }
  for (usize i = 1; i < dir.size(); ++i) {
    if (dir[i] != path::DEFAULT_PATH_SEPARATOR) {
      continue;
    }
    const std::string_view prefix = dir.substr(0, i);
    if (prefix.ends_with(':')) {
      continue;
    }
    if (!io::create_directory(std::string(prefix))) {
      const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                     "cannot create directory '{}'", prefix);
      (void)index;
      return base::make_err(diag::Reported{});
    }
  }
  if (!io::create_directory(std::string(dir))) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot create directory '{}'", dir);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

}  // namespace

// Runs type checking, lowering, and borrow checking over a
// resolved tree.
base::Result<lower::LoweredPackage, diag::Reported> compile_tree(
    PipelineContext& ctx,
    analyzer::ModuleTree tree) {
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyzer::check_package(tree, TARGET_WIDTH, ctx.ast, ctx.bag);
  if (checked.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  base::Result<lower::LoweredPackage, diag::Reported> lowered =
      lower::lower_package(std::move(checked).unwrap(), TARGET_WIDTH, ctx.ast,
                           ctx.strings, ctx.bag);
  if (lowered.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lower::LoweredPackage package = std::move(lowered).unwrap();
  if (borrow::check_borrows(package, ctx.bag).is_err() ||
      ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(std::move(package));
}

// Emits one relocatable object for lowered IR.
base::Result<void, diag::Reported> emit_package_object(
    PipelineContext& ctx,
    lower::LoweredPackage& package,
    bool optimize,
    const std::string& output_path) {
  llvm::LLVMContext context;
  // TODO: Add optimization level enum
  (void)optimize;
  auto module = std::make_unique<llvm::Module>("alcy_module", context);
  codegen_llvm::LlvmIrEmitter emitter(module.get(), std::move(package.storage),
                                      &ctx.strings, TARGET_WIDTH);
  std::move(emitter).emit();
  base::Result<std::vector<u8>, codegen_llvm::ObjectEmitError> emitted =
      codegen_llvm::emit_object(*module, "");
  if (emitted.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot emit object '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const std::vector<u8>& buffer = std::move(emitted).unwrap();
  if (ensure_parent_directories(ctx, output_path).is_err()) {
    return base::make_err(diag::Reported{});
  }
  if (!io::write_file(buffer, output_path)) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot write object '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

// Links one object plus the staged runtime into an executable
// through the system linker. Empty selects the default toolchain driver.
base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    std::string_view linker,
    const std::string& object_path,
    const std::string& runtime_path,
    const std::string& exe_path) {
  const std::string driver = linker.empty() ? "clang" : std::string(linker);

  base::Result<i32, SpawnError> linked =
      run_command({driver, object_path, runtime_path, "-o", exe_path});
  if (linked.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_LINK_ERROR,
                                   "cannot run the system compiler");
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const i32 code = std::move(linked).unwrap();
  if (code != 0) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_LINK_ERROR,
                                   "linking '{}' failed", exe_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

// Single-file executable build (package builds stay on
// discovery until wires them). Runs the full frontend plus
// borrow checking, then lowers and emits one relocatable object.
base::Result<void, diag::Reported> build_single_file(PipelineContext& ctx,
                                                     std::string_view target,
                                                     std::string_view output,
                                                     bool optimize,
                                                     std::string_view linker) {
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target);
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot read '{}'", target);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> prelude =
      std_prelude(ctx);
  if (prelude.is_err()) {
    return base::make_err(diag::Reported{});
  }
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      analyzer::resolve_modules(root, {&single_input, 1}, "", ctx.sources,
                                ctx.ast, ctx.bag, std::move(prelude).unwrap());
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  base::Result<lower::LoweredPackage, diag::Reported> package =
      compile_tree(ctx, std::move(tree).unwrap());
  if (package.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
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
    return emit_package_object(ctx, lowered, optimize, output_path);
  }
  io::TempDir scratch = io::TempDir::create_unique("alcy_build_");
  const std::string object_path = scratch.join("main.o");
  if (emit_package_object(ctx, lowered, optimize, object_path).is_err() ||
      stage_runtime(scratch, ctx.bag).is_err()) {
    return base::make_err(diag::Reported{});
  }
  const std::string runtime_path = scratch.join(runtime_source_name());
  return link_executable(ctx, linker, object_path, runtime_path, output_path);
}

base::Result<void, diag::Reported> build_package(PipelineContext& ctx,
                                                 const path::Path& root,
                                                 source::FileId manifest_file,
                                                 std::string_view manifest_name,
                                                 std::string_view output,
                                                 bool optimize,
                                                 std::string_view linker) {
  base::Result<BinTarget, diag::Reported> target =
      resolve_package_target(ctx, root, manifest_file, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  BinTarget resolved = std::move(target).unwrap();
  base::Result<lower::LoweredPackage, diag::Reported> package =
      compile_tree(ctx, resolved.tree);
  if (package.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
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
  io::TempDir scratch = io::TempDir::create_unique("alcy_build_");
  const std::string object_path = scratch.join("main.o");
  if (emit_package_object(ctx, lowered, optimize, object_path).is_err() ||
      stage_runtime(scratch, ctx.bag).is_err()) {
    return base::make_err(diag::Reported{});
  }
  const std::string runtime_path = scratch.join(runtime_source_name());
  return link_executable(ctx, linker, object_path, runtime_path, exe_path);
}

}  // namespace pipeline

