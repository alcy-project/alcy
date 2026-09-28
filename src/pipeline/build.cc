// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/build.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/llvm_object_emitter.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/emit_mode.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/runtime_stage.h"
#include "pipeline/spawn.h"
#include "pipeline/target.h"
#include "source/source.h"

namespace pipeline {

base::Result<lower::LoweredPackage, diag::Reported> compile_tree(
    PipelineContext& ctx,
    analyzer::ModuleTree tree) {
  base::Result<FrontendOutput, diag::Reported> out = run_frontend(ctx, tree);
  if (out.is_err()) {
    return base::make_err(diag::Reported{});
  }
  FrontendOutput done = std::move(out).unwrap();
  return base::make_ok(std::move(done.package));
}

// The extension an unnamed output gets. It follows the mode rather than
// the file kind, because the mode is what the caller asked for: naming an
// object `main.bin` and a module `main` are both fine, and the extension
// is the default rather than the switch.
std::string suffix_for(EmitMode mode) {
  switch (mode) {
    case EmitMode::Object: return ".o";
    case EmitMode::LlvmIr: return ".ll";
    case EmitMode::Executable: break;
  }
  return std::string(exe_suffix());
}

base::Result<void, diag::Reported> emit_package_object(
    PipelineContext& ctx,
    lower::LoweredPackage& package,
    bool optimize,
    const std::string& output_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-object",
                                           "backend");
  llvm::LLVMContext context;
  auto module = std::make_unique<llvm::Module>("alcy_module", context);
  codegen_llvm::LlvmIrEmitter emitter(module.get(), std::move(package.storage),
                                      &ctx.strings, TARGET_WIDTH);
  std::move(emitter).emit();
  base::Result<std::vector<u8>, codegen_llvm::ObjectEmitError> emitted =
      codegen_llvm::emit_object(*module, "", optimize);
  if (emitted.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot emit object '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const std::vector<u8>& buffer = std::move(emitted).unwrap();
  base::Result<path::Path, path::PathError> parsed =
      path::Path::from_native(output_path);
  if (parsed.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "invalid output path '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const path::Path parent = std::move(parsed).unwrap().parent();
  if (ensure_directories(ctx, parent.as_view()).is_err() ||
      !io::write_file(buffer, output_path)) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot write object '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

base::Result<void, diag::Reported> emit_package_ir(
    PipelineContext& ctx,
    lower::LoweredPackage& package,
    const std::string& output_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-ir", "backend");
  llvm::LLVMContext context;
  auto module = std::make_unique<llvm::Module>("alcy_module", context);
  codegen_llvm::LlvmIrEmitter emitter(module.get(), std::move(package.storage),
                                      &ctx.strings, TARGET_WIDTH);
  std::move(emitter).emit();
  const std::string ir = codegen_llvm::emit_ir(*module);

  base::Result<path::Path, path::PathError> parsed =
      path::Path::from_native(output_path);
  if (parsed.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "invalid output path '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const path::Path parent = std::move(parsed).unwrap().parent();
  const std::vector<u8> buffer(ir.begin(), ir.end());
  if (ensure_directories(ctx, parent.as_view()).is_err() ||
      !io::write_file(buffer, output_path)) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot write ir '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    std::string_view linker,
    const std::string& object_path,
    const std::string& runtime_path,
    const std::string& exe_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "link", "backend");
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

base::Result<void, diag::Reported> emit_output(PipelineContext& ctx,
                                               lower::LoweredPackage& lowered,
                                               bool optimize,
                                               std::string_view linker,
                                               EmitMode mode,
                                               const std::string& output_path) {
  if (mode == EmitMode::Object) {
    return emit_package_object(ctx, lowered, optimize, output_path);
  }
  if (mode == EmitMode::LlvmIr) {
    return emit_package_ir(ctx, lowered, output_path);
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

// Single-file build: runs the full frontend over one source file, then
// emits an object and links it with the staged runtime.
base::Result<void, diag::Reported> build_single_file(PipelineContext& ctx,
                                                     std::string_view target,
                                                     std::string_view output,
                                                     bool optimize,
                                                     std::string_view linker,
                                                     EmitMode mode,
                                                     const StdSelection& selection) {
  base::Result<source::FileId, source::SourceError> file = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "load", "frontend");
    return ctx.sources.load(target);
  }();
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot read '{}'", target);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const source::FileId root = std::move(file).unwrap();
  std::string output_path =
      output.empty() ? std::string(target) : std::string(output);
  if (output.empty()) {
    // The target spells a source file, so an extension is present.
    const usize dot = output_path.rfind('.');
    DCHECK(dot != std::string::npos);
    output_path.replace(dot, std::string::npos, suffix_for(mode));
  }
  return build_single_root(ctx, root, output_path, optimize, linker, mode,
                             selection);
}

base::Result<void, diag::Reported> build_single_root(PipelineContext& ctx,
                                                     source::FileId root,
                                                     std::string_view output,
                                                     bool optimize,
                                                     std::string_view linker,
                                                     EmitMode mode,
                                                     const StdSelection& selection) {
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      front_end_root(ctx, root, selection);
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  base::Result<lower::LoweredPackage, diag::Reported> package =
      compile_tree(ctx, std::move(tree).unwrap());
  if (package.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lower::LoweredPackage lowered = std::move(package).unwrap();
  return emit_output(ctx, lowered, optimize, linker, mode, std::string(output));
}

base::Result<void, diag::Reported> build_package(PipelineContext& ctx,
                                                 const path::Path& root,
                                                 source::FileId manifest_file,
                                                 std::string_view manifest_name,
                                                 std::string_view output,
                                                 bool optimize,
                                                 std::string_view linker,
                                                 EmitMode mode) {
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
  // An executable goes where the manifest says builds go; the other two
  // are inspection outputs, so they land beside the manifest unless the
  // caller named a path.
  const path::Path out_dir = root.join(path::DEFAULT_OUT_DIR);
  std::string output_path;
  if (output.empty()) {
    if (mode == EmitMode::Executable) {
      if (ensure_directories(ctx, out_dir.as_view()).is_err()) {
        return base::make_err(diag::Reported{});
      }
      output_path =
          out_dir
              .join(std::string(resolved.bin_name) + std::string(exe_suffix()))
              .as_view();
    } else {
      output_path = root.join(std::string(resolved.bin_name) + suffix_for(mode))
                        .as_view();
    }
  } else {
    output_path = std::string(output);
  }
  return emit_output(ctx, lowered, optimize, linker, mode, output_path);
}

}  // namespace pipeline

