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
#include "codegen/backend.h"
#include "codegen/target.h"
#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/messages.h"
#include "lowering/lowering.h"
#include "path/path.h"
#include "pipeline/backend_emit.h"
#include "pipeline/diag_code.h"
#include "pipeline/embedded_lld.h"
#include "pipeline/emit_mode.h"
#include "pipeline/frontend.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/spawn.h"
#include "pipeline/std_select.h"
#include "pipeline/target.h"
#include "source/source.h"

namespace pipeline {

base::Result<lowering::LoweredPackage, diag::Reported> compile_tree(
    PipelineContext& ctx,
    analyzer::ModuleTree tree) {
  base::Result<FrontendOutput, diag::Reported> out = run_frontend(ctx, tree);
  if (out.is_err()) {
    return base::make_err(diag::Reported{});
  }
  FrontendOutput done = std::move(out).unwrap();
  return base::make_ok(std::move(done.package));
}

namespace {

// The extension an unnamed output gets. It follows the mode rather than
// the file kind, because the mode is what the caller asked for: naming an
// object `main.bin` and a module `main` are both fine, and the extension
// is the default rather than the switch.
std::string suffix_for(const codegen::Target& target, EmitMode mode) {
  switch (mode) {
    case EmitMode::Object: return ".o";
    case EmitMode::LlvmIr: return ".ll";
    case EmitMode::LlvmBitcode: return ".bc";
    case EmitMode::Executable: break;
  }
  // A wasm executable is the final module itself; every other target's
  // executable suffix is the platform's.
  return target.is_wasm() ? std::string(".wasm") : std::string(exe_suffix());
}

// The last separator in `name`, or npos. Both spellings are separators on
// Windows because the target is what the user typed; on POSIX a backslash
// is an ordinary character in a name.
usize last_separator(std::string_view name) {
#if BUILD_FLAG(IS_OS_WIN)
  return name.find_last_of("/\\");
#else
  return name.find_last_of(path::DEFAULT_PATH_SEPARATOR);
#endif
}

// The artifact an unnamed target gets: its own name with the mode's
// suffix in place of its extension. A dot before the last separator is
// part of a directory name rather than an extension, and a mode with no
// suffix - the executable one on POSIX - has nothing to replace an
// extension with, so a target without one has to be named by the caller.
// The alternative, appending nothing, would write the artifact over the
// source at the same path.
base::Result<std::string, diag::Reported> default_output_path(
    PipelineContext& ctx,
    std::string_view target,
    EmitMode mode) {
  const usize separator = last_separator(target);
  const usize dot = target.rfind('.');
  const bool has_extension =
      dot != std::string_view::npos &&
      (separator == std::string_view::npos || dot > separator);
  std::string output(target);
  if (has_extension) {
    output.replace(dot, std::string::npos, suffix_for(ctx.target, mode));
    return base::make_ok(std::move(output));
  }
  std::string suffix = suffix_for(ctx.target, mode);
  if (suffix.empty()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotDeriveOutput>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoOutputName,
        target);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(std::move(output) + suffix);
}

}  // namespace

// Maps a backend failure to the pipeline's message for it. Anything
// finer is already in the bag; what lands here is the one line the
// command prints and the code it carries.
base::Result<void, diag::Reported> report_emit_failure(
    PipelineContext& ctx,
    codegen::EmitError error,
    codegen::OutputKind kind,
    const std::string& output_path) {
  // A backend that already reported the construct it could not encode has
  // said what went wrong; a second line saying the output could not be
  // written only buries the span.
  if (ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  switch (error) {
    case codegen::EmitError::UnknownTarget: {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineUnknownTarget>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::UnknownTarget,
          ctx.target.triple);
      (void)index;
      break;
    }
    case codegen::EmitError::CannotOptimize: {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotOptimize>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError);
      (void)index;
      break;
    }
    case codegen::EmitError::NoOptimizer: {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineBackendNoOptimizer>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoOptimizer,
          codegen::backend_name(ctx.backend));
      (void)index;
      break;
    }
    case codegen::EmitError::Unsupported: {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineBackendUnsupported>(
          diag::Severity::Error, diag::Stage::Pipeline,
          DiagCode::UnsupportedOutput, codegen::backend_name(ctx.backend));
      (void)index;
      break;
    }
    case codegen::EmitError::CannotEmit: {
      if (kind == codegen::OutputKind::Module) {
        const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotEmitModule>(
            diag::Severity::Error, diag::Stage::Pipeline,
            DiagCode::CannotEmitModule, output_path);
        (void)index;
      } else {
        const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotEmitObject>(
            diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
            output_path);
        (void)index;
      }
      break;
    }
  }
  return base::make_err(diag::Reported{});
}

// Writes bytes to `output_path`, whose parent directory is the caller's
// to have made: every mode writes to one path, and the path is chosen
// before the mode is acted on.
base::Result<void, diag::Reported> write_output(PipelineContext& ctx,
                                                std::string_view what,
                                                const std::string& output_path,
                                                std::span<const u8> bytes) {
  if (!io::write_file(bytes, output_path)) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotWrite>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError, what,
        output_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

// Emits one lowered package through the backend the context names. The
// kind is what the backend is asked for; `what` is the noun a write
// failure uses ("object", "ir", "wasm module").
base::Result<void, diag::Reported> emit_package_output(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    bool optimize,
    const std::string& output_path,
    bool is_lib,
    codegen::OutputKind kind,
    std::string_view what) {
  if (ctx.backend == codegen::Backend::None) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineNoBackend>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoBackend);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (!codegen::backend_available(ctx.backend)) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineBackendNotBuilt>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoBackend,
        codegen::backend_name(ctx.backend));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (!backend_supports(ctx.backend, kind, ctx.target)) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineBackendUnsupported>(
        diag::Severity::Error, diag::Stage::Pipeline,
        DiagCode::UnsupportedOutput, codegen::backend_name(ctx.backend));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (ctx.freestanding && !is_lib) {
    // The exit sequence is written per architecture, so a target with
    // no sequence has no freestanding entry to emit. Refusing here
    // names the target instead of leaving the emitter to guess.
    const std::string_view triple = ctx.target.triple;
    const std::string_view arch = triple.substr(0, triple.find('-'));
    if (arch != "x86_64" && arch != "aarch64" && arch != "riscv64") {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineFreestandingTarget>(
          diag::Severity::Error, diag::Stage::Pipeline,
          DiagCode::UnsupportedOutput, ctx.target.triple);
      (void)index;
      return base::make_err(diag::Reported{});
    }
  }
  codegen::EmitRequest request{
      .storage = std::move(package.storage),
      .instr_spans = package.instr_spans,
      .strings = &ctx.strings,
      .bag = &ctx.bag,
      .target = ctx.target,
      .emit_entry = !is_lib,
      .freestanding = ctx.freestanding,
      .optimize = optimize,
      .kind = kind,
      .profiler = ctx.profiler,
  };
  base::Result<std::vector<u8>, codegen::EmitError> bytes =
      emit_with_backend(ctx.backend, std::move(request));
  if (bytes.is_err()) {
    return report_emit_failure(ctx, std::move(bytes).unwrap_err(), kind,
                               output_path);
  }
  return write_output(ctx, what, output_path, std::move(bytes).unwrap());
}

base::Result<void, diag::Reported> emit_package_object(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    bool optimize,
    const std::string& output_path,
    bool is_lib) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-object",
                                           "backend");
  return emit_package_output(ctx, package, optimize, output_path, is_lib,
                             codegen::OutputKind::Object, "object");
}

base::Result<void, diag::Reported> emit_package_ir(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    bool optimize,
    const std::string& output_path,
    bool is_lib) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-ir", "backend");
  return emit_package_output(ctx, package, optimize, output_path, is_lib,
                             codegen::OutputKind::Text, "ir");
}

base::Result<void, diag::Reported> emit_package_bitcode(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    bool optimize,
    const std::string& output_path,
    bool is_lib) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-bitcode",
                                           "backend");
  return emit_package_output(ctx, package, optimize, output_path, is_lib,
                             codegen::OutputKind::Bitcode, "bitcode");
}

base::Result<void, diag::Reported> emit_package_module(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    bool optimize,
    const std::string& output_path,
    bool is_lib) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-wasm",
                                           "backend");
  return emit_package_output(ctx, package, optimize, output_path, is_lib,
                             codegen::OutputKind::Module, "module");
}

base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    LinkOptions link,
    const std::string& object_path,
    const std::string& exe_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "link", "backend");
  // A configured linker replaces the embedded one; the embedded one runs
  // in-process where the build has it and the host's startup inputs are
  // found.
  if (link.driver.empty() && embedded_lld_ready()) {
    return link_with_embedded_lld(ctx, link, object_path, exe_path);
  }
  const std::string driver =
      link.driver.empty() ? "clang" : std::string(link.driver);
  // The driver's own arguments follow the object, where a library is
  // resolved against it, and stop short of the output, which stays the
  // last word.
  std::vector<std::string> argv;
  argv.reserve(6 + link.args.size() + 2);
  argv.emplace_back(driver);
  argv.emplace_back(object_path);
  for (std::string_view argument : link.args) {
    argv.emplace_back(argument);
  }
  if (link.freestanding) {
    // No startup files and no C library; the object's `_start` is the
    // entry. `-static` keeps the driver from asking for a loader the
    // program has no symbols for (ADR-0052).
    argv.emplace_back("-nostdlib");
    argv.emplace_back("-static");
    argv.emplace_back("-Wl,-e,_start");
  }
  argv.emplace_back("-o");
  argv.emplace_back(exe_path);

  base::Result<i32, SpawnError> linked = run_command(argv);
  if (linked.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotRunSystemCompiler>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::LinkError);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const i32 code = std::move(linked).unwrap();
  if (code != 0) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineLinkFailed>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::LinkError,
        exe_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

base::Result<std::string, diag::Reported> emit_output(
    PipelineContext& ctx,
    lowering::LoweredPackage& lowered,
    bool optimize,
    LinkOptions link,
    EmitMode mode,
    const std::string& output_path,
    bool is_lib) {
  // One path, so its parent is made once and every mode agrees about it.
  // The linker creates no directories of its own, which left
  // `alcy build -o out/app` failing where the same `-o` for an object
  // worked.
  base::Result<path::Path, path::PathError> parsed =
      path::Path::from_native(output_path);
  if (parsed.is_err()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineInvalidOutputPath>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        output_path);
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
      return emit_package_object(ctx, lowered, optimize, output_path, is_lib);
    }
    if (mode == EmitMode::LlvmIr) {
      return emit_package_ir(ctx, lowered, optimize, output_path, is_lib);
    }
    if (mode == EmitMode::LlvmBitcode) {
      return emit_package_bitcode(ctx, lowered, optimize, output_path, is_lib);
    }
    // Executable: a backend that writes objects hands them to the linker;
    // one whose output is a final module writes that module and stops.
    if (!backend_supports(ctx.backend, codegen::OutputKind::Object,
                          ctx.target)) {
      return emit_package_module(ctx, lowered, optimize, output_path, is_lib);
    }
    io::TempDir scratch = io::TempDir::create_unique("alcy_build_");
    const std::string object_path = scratch.join("main.o");
    if (emit_package_object(ctx, lowered, optimize, object_path, is_lib)
            .is_err()) {
      return base::make_err(diag::Reported{});
    }
    return link_executable(ctx, link, object_path, output_path);
  }();
  if (written.is_err()) {
    return base::make_err(std::move(written).unwrap_err());
  }
  return base::make_ok(output_path);
}

// Single-file build: runs the full frontend over one source file, then
// emits an object and links it into an executable.
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
    const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotRead>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        target);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  const source::FileId root = std::move(file).unwrap();
  std::string output_path(output);
  if (output.empty()) {
    base::Result<std::string, diag::Reported> derived =
        default_output_path(ctx, target, mode);
    if (derived.is_err()) {
      return base::make_err(diag::Reported{});
    }
    output_path = std::move(derived).unwrap();
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
  base::Result<lowering::LoweredPackage, diag::Reported> package =
      compile_tree(ctx, std::move(tree).unwrap());
  if (package.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lowering::LoweredPackage lowered = std::move(package).unwrap();
  return emit_output(ctx, lowered, optimize, link, mode, std::string(output),
                     false);
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
  base::Result<std::vector<PackageTarget>, diag::Reported> targets =
      resolve_package_targets(ctx, root, manifest_file, manifest_name,
                              TargetScope::All);
  if (targets.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  std::vector<PackageTarget> resolved = std::move(targets).unwrap();
  if (resolved.size() > 1 && !output.empty()) {
    // One `-o` cannot name two artifacts.
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineMultipleTargetsWithOutput>(
            diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets,
            output);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  // An executable goes where the manifest says builds go; the other two
  // are inspection outputs, so they land beside the manifest unless the
  // caller named a path.
  const path::Path out_dir = root.join(path::DEFAULT_OUT_DIR);
  struct PlannedTarget {
    PackageTarget* target;
    EmitMode mode;
    std::string path;
  };
  std::vector<PlannedTarget> planned;
  planned.reserve(resolved.size());
  for (PackageTarget& target : resolved) {
    // A library has no entry to link, so an executable request becomes
    // an object one.
    const EmitMode target_mode =
        target.is_lib && mode == EmitMode::Executable ? EmitMode::Object : mode;
    std::string output_path;
    if (output.empty()) {
      // Everything a package build writes goes to the directory the
      // scaffold's own `.gitignore` names, whatever the mode. An object or
      // a module beside the manifest landed outside the one region the
      // compiler told git to ignore, so `git status` reported the build's
      // own output as untracked.
      output_path = out_dir
                        .join(std::string(target.name) +
                              suffix_for(ctx.target, target_mode))
                        .as_view();
    } else {
      output_path = std::string(output);
    }
    planned.push_back({&target, target_mode, std::move(output_path)});
  }
  for (usize i = 0; i < planned.size(); ++i) {
    for (usize j = i + 1; j < planned.size(); ++j) {
      if (planned[i].path != planned[j].path) {
        continue;
      }
      // Two targets sharing a default name write the same file in every
      // mode but the executable one, and the second silently wins. Refuse
      // the pair before compiling either, naming what collided.
      const PackageTarget& one = *planned[i].target;
      const PackageTarget& other = *planned[j].target;
      const PackageTarget& bin = one.is_lib ? other : one;
      const PackageTarget& lib = one.is_lib ? one : other;
      const u32 index = ctx.bag.emit<i18n::Key::PipelineTargetOutputsCollide>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets,
          bin.name, lib.name, planned[i].path);
      (void)index;
      return base::make_err(diag::Reported{});
    }
  }
  // The cli reports one artifact per build, so a package with several
  // targets names the first: the binary, resolved ahead of the library.
  bool failed = false;
  std::string first_output;
  for (PlannedTarget& planned_target : planned) {
    PackageTarget& target = *planned_target.target;
    base::Result<lowering::LoweredPackage, diag::Reported> package =
        compile_tree(ctx, target.tree);
    if (package.is_err() || ctx.bag.has_errors()) {
      failed = true;
      break;
    }
    lowering::LoweredPackage lowered = std::move(package).unwrap();
    base::Result<std::string, diag::Reported> written =
        emit_output(ctx, lowered, optimize, link, planned_target.mode,
                    planned_target.path, target.is_lib);
    if (written.is_err() || ctx.bag.has_errors()) {
      failed = true;
      break;
    }
    if (first_output.empty()) {
      first_output = std::move(written).unwrap();
    }
  }
  // Each target analyzes the shared modules with its own root, so one
  // warning can arrive once per tree; the run renders it once, whether
  // or not a later target failed.
  ctx.bag.dedup();
  if (failed || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(first_output);
}

}  // namespace pipeline

