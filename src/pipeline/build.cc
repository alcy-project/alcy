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
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/runtime_stage.h"
#include "pipeline/spawn.h"
#include "pipeline/std_select.h"
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
    case EmitMode::LlvmBitcode: return ".bc";
    case EmitMode::Executable: break;
  }
  return std::string(exe_suffix());
}

// The module a lowered package becomes, and the context it lives in.
//
// The two travel together because a module is invalid once its context
// goes, and the context is neither copyable nor movable, so the caller
// constructs this and has it filled rather than receiving one back.
struct EmittedModule {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;

  // Builds the module and, when asked for it, optimizes it. This is the
  // only place that decides: the optimization belongs to the module, not
  // to whichever backend goes on to consume it, and a backend that ran
  // the pipeline on its own left every other consumer reading
  // unoptimized IR.
  base::Result<void, diag::Reported> build(PipelineContext& ctx,
                                           lower::LoweredPackage& package,
                                           bool optimize) {
    module = std::make_unique<llvm::Module>("alcy_module", context);
    codegen_llvm::LlvmIrEmitter emitter(
        module.get(), std::move(package.storage), &ctx.strings, TARGET_WIDTH);
    std::move(emitter).emit();
    if (!optimize) {
      return base::make_ok();
    }
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "optimize",
                                             "backend");
    if (codegen_llvm::optimize_module(*module, "").is_err()) {
      const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                     "cannot optimize the module");
      (void)index;
      return base::make_err(diag::Reported{});
    }
    return base::make_ok();
  }
};

// Writes bytes to `output_path`, whose parent directory is the caller's
// to have made: every mode writes to one path, and the path is chosen
// before the mode is acted on.
base::Result<void, diag::Reported> write_output(PipelineContext& ctx,
                                                std::string_view what,
                                                const std::string& output_path,
                                                std::span<const u8> bytes) {
  if (!io::write_file(bytes, output_path)) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot write {} '{}'", what, output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

base::Result<void, diag::Reported> emit_package_object(
    PipelineContext& ctx,
    lower::LoweredPackage& package,
    bool optimize,
    const std::string& output_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-object",
                                           "backend");
  EmittedModule emitted;
  if (emitted.build(ctx, package, optimize).is_err()) {
    return base::make_err(diag::Reported{});
  }
  base::Result<std::vector<u8>, codegen_llvm::ObjectEmitError> object =
      codegen_llvm::emit_object(*emitted.module, "");
  if (object.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot emit object '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return write_output(ctx, "object", output_path, std::move(object).unwrap());
}

base::Result<void, diag::Reported> emit_package_ir(
    PipelineContext& ctx,
    lower::LoweredPackage& package,
    bool optimize,
    const std::string& output_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-ir", "backend");
  EmittedModule emitted;
  if (emitted.build(ctx, package, optimize).is_err()) {
    return base::make_err(diag::Reported{});
  }
  const std::string ir = codegen_llvm::emit_ir(*emitted.module);
  return write_output(
      ctx, "ir", output_path,
      std::span<const u8>(reinterpret_cast<const u8*>(ir.data()), ir.size()));
}

base::Result<void, diag::Reported> emit_package_bitcode(
    PipelineContext& ctx,
    lower::LoweredPackage& package,
    bool optimize,
    const std::string& output_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-bitcode",
                                           "backend");
  EmittedModule emitted;
  if (emitted.build(ctx, package, optimize).is_err()) {
    return base::make_err(diag::Reported{});
  }
  const std::vector<u8> bytes = codegen_llvm::emit_bitcode(*emitted.module);
  return write_output(ctx, "bitcode", output_path, bytes);
}

base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    LinkOptions link,
    const std::string& object_path,
    const std::string& runtime_path,
    const std::string& exe_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "link", "backend");
  const std::string driver =
      link.driver.empty() ? "clang" : std::string(link.driver);
  // The driver's own arguments follow both objects, where a library is
  // resolved against them, and stop short of the output, which stays the
  // last word.
  std::vector<std::string> argv;
  argv.reserve(3 + link.args.size() + 2);
  argv.emplace_back(driver);
  argv.emplace_back(object_path);
  argv.emplace_back(runtime_path);
  for (std::string_view argument : link.args) {
    argv.emplace_back(argument);
  }
  argv.emplace_back("-o");
  argv.emplace_back(exe_path);

  base::Result<i32, SpawnError> linked = run_command(argv);
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

base::Result<std::string, diag::Reported> emit_output(
    PipelineContext& ctx,
    lower::LoweredPackage& lowered,
    bool optimize,
    LinkOptions link,
    EmitMode mode,
    const std::string& output_path) {
  // One path, so its parent is made once and every mode agrees about it.
  // The linker creates no directories of its own, which left
  // `alcy build -o out/app` failing where the same `-o` for an object
  // worked.
  base::Result<path::Path, path::PathError> parsed =
      path::Path::from_native(output_path);
  if (parsed.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "invalid output path '{}'", output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (ensure_directories(ctx, std::move(parsed).unwrap().parent().as_view())
          .is_err()) {
    return base::make_err(diag::Reported{});
  }

  base::Result<void, diag::Reported> written =
      [&]() -> base::Result<void, diag::Reported> {
    if (mode == EmitMode::Object) {
      return emit_package_object(ctx, lowered, optimize, output_path);
    }
    if (mode == EmitMode::LlvmIr) {
      return emit_package_ir(ctx, lowered, optimize, output_path);
    }
    if (mode == EmitMode::LlvmBitcode) {
      return emit_package_bitcode(ctx, lowered, optimize, output_path);
    }
    io::TempDir scratch = io::TempDir::create_unique("alcy_build_");
    const std::string object_path = scratch.join("main.o");
    if (emit_package_object(ctx, lowered, optimize, object_path).is_err() ||
        stage_runtime(scratch, ctx.bag).is_err()) {
      return base::make_err(diag::Reported{});
    }
    const std::string runtime_path = scratch.join(runtime_source_name());
    return link_executable(ctx, link, object_path, runtime_path, output_path);
  }();
  if (written.is_err()) {
    return base::make_err(std::move(written).unwrap_err());
  }
  return base::make_ok(output_path);
}

// Single-file build: runs the full frontend over one source file, then
// emits an object and links it with the staged runtime.
base::Result<std::string, diag::Reported> build_single_file(
    PipelineContext& ctx,
    std::string_view target,
    std::string_view output,
    bool optimize,
    LinkOptions link,
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
  return build_single_root(ctx, root, output_path, optimize, link, mode,
                           selection);
}

base::Result<std::string, diag::Reported> build_single_root(
    PipelineContext& ctx,
    source::FileId root,
    std::string_view output,
    bool optimize,
    LinkOptions link,
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
  return emit_output(ctx, lowered, optimize, link, mode, std::string(output));
}

base::Result<std::string, diag::Reported> build_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    std::string_view output,
    bool optimize,
    LinkOptions link,
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
    // Everything a package build writes goes to the directory the
    // scaffold's own `.gitignore` names, whatever the mode. An object or
    // a module beside the manifest landed outside the one region the
    // compiler told git to ignore, so `git status` reported the build's
    // own output as untracked.
    output_path =
        out_dir.join(std::string(resolved.bin_name) + suffix_for(mode))
            .as_view();
  } else {
    output_path = std::string(output);
  }
  return emit_output(ctx, lowered, optimize, link, mode, output_path);
}

}  // namespace pipeline

