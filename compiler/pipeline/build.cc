// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/build.h"

#include <algorithm>
#include <memory>
#include <optional>
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
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/binary_format.h"
#include "ir/serialize.h"
#include "ir/storage.h"
#include "ir/text_dump.h"
#include "ir/verifier.h"
#include "ir/write_input.h"
#include "lowering/lowering.h"
#include "opt/opt.h"
#include "overlay/overlay.h"
#include "path/path.h"
#include "pipeline/backend_emit.h"
#include "pipeline/call_sites.h"
#include "pipeline/diag_code.h"
#include "pipeline/embedded_lld.h"
#include "pipeline/emit_mode.h"
#include "pipeline/frontend.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/profile.h"
#include "pipeline/spawn.h"
#include "pipeline/std_select.h"
#include "pipeline/target.h"
#include "pkg/manifest.h"
#include "pkg/profile.h"
#include "source/source.h"

namespace pipeline {

base::Result<lowering::LoweredPackage, diag::Reported> compile_tree(
    PipelineContext& ctx,
    analyzer::ModuleTree tree,
    bool optimize,
    bool needs_entry,
    std::string_view entry_name) {
  const bool has_main = !needs_entry || has_free_main(ctx, tree);
  base::Result<FrontendOutput, diag::Reported> out = run_frontend(ctx, tree);
  if (out.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  if (!has_main) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineNoEntryPoint>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoEntryPoint,
        entry_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  FrontendOutput done = std::move(out).unwrap();
  // The release passes run after borrow checking, so the ownership they
  // leave behind still holds, and both backends - including the direct
  // one with no middle end - see the same optimized IR.
  if (optimize) {
    base::Result<ir::VerifiedStorage, ir::VerificationError> optimized =
        opt::optimize(std::move(done.package.storage));
    if (optimized.is_err()) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineOptimizeFailed>(
          diag::Severity::Error, diag::Stage::Pipeline,
          DiagCode::OptimizeFailed);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    done.package.storage = std::move(optimized).unwrap();
  }
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
    case EmitMode::Ir: return ".ir";
    case EmitMode::IrBinary: return ".irb";
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

// Refuses an emission the context cannot answer: no backend, a backend
// this build does not carry, a kind the backend does not support, and a
// freestanding executable on a target with no exit sequence. Every
// object path runs it, single-part or split, so a refusal names the
// reason rather than reaching an emitter that would guess.
base::Result<void, diag::Reported> check_emission(PipelineContext& ctx,
                                                  codegen::OutputKind kind,
                                                  bool is_lib) {
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
  return base::make_ok();
}

// Emits one lowered package through the backend the context names. The
// kind is what the backend is asked for; `what` is the noun a write
// failure uses ("object", "ir", "wasm module").
base::Result<void, diag::Reported> emit_package_output(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildProfile profile,
    const std::string& output_path,
    bool is_lib,
    codegen::OutputKind kind,
    std::string_view what) {
  if (check_emission(ctx, kind, is_lib).is_err()) {
    return base::make_err(diag::Reported{});
  }
  // The rendered sites serve both the checks' aborts and a backtrace's
  // call sites; one rendering fills both. The table lives in this frame
  // and the request only borrows it, which lasts exactly as long as the
  // emission below.
  const bool want_sites =
      profile.overflow_checks || profile.on_panic == pkg::OnPanic::Backtrace;
  std::vector<str::StringPoolId> site_names;
  if (want_sites) {
    site_names = render_instr_sites(ctx, package.instr_spans);
  }
  // The build's checks ride beside the IR (ADR-0068): one walk turns
  // the profile into per-instruction entries the backend realizes. A
  // build with the checks off computes nothing.
  overlay::Overlay checks;
  if (profile.overflow_checks) {
    checks = overlay::build(*package.storage, true, site_names);
  }
  codegen::EmitRequest request{
      .storage = std::move(package.storage),
      .instr_spans = package.instr_spans,
      .instr_site_names = profile.on_panic == pkg::OnPanic::Backtrace
                              ? std::span<const str::StringPoolId>(site_names)
                              : std::span<const str::StringPoolId>{},
      .strings = &ctx.strings,
      .bag = &ctx.bag,
      .target = ctx.target,
      .emit_entry = !is_lib,
      .freestanding = ctx.freestanding,
      .backtrace = profile.on_panic == pkg::OnPanic::Backtrace,
      .optimize = profile.optimize,
      .overlay = profile.overflow_checks ? &checks : nullptr,
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

u32 codegen_parts(const lowering::LoweredPackage& package) {
  constexpr u32 FUNCTIONS_PER_PART = 128;
  constexpr u32 MAX_PARTS = 16;
  const u32 functions = static_cast<u32>(package.storage->functions().size());
  return std::clamp(functions / FUNCTIONS_PER_PART, 1u, MAX_PARTS);
}

base::Result<std::vector<std::string>, diag::Reported> emit_package_objects(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildProfile profile,
    const io::TempDir& scratch,
    bool is_lib,
    u32 parts) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-object",
                                           "backend");
  if (check_emission(ctx, codegen::OutputKind::Object, is_lib).is_err()) {
    return base::make_err(diag::Reported{});
  }
  // The rendered sites serve both the checks' aborts and a backtrace's
  // call sites; one rendering fills both. The table lives in this frame
  // and the request only borrows it, which lasts exactly as long as the
  // emission below.
  const bool want_sites =
      profile.overflow_checks || profile.on_panic == pkg::OnPanic::Backtrace;
  std::vector<str::StringPoolId> site_names;
  if (want_sites) {
    site_names = render_instr_sites(ctx, package.instr_spans);
  }
  // The build's checks ride beside the IR (ADR-0068); the split
  // emission carries the same table as the whole one.
  overlay::Overlay checks;
  if (profile.overflow_checks) {
    checks = overlay::build(*package.storage, true, site_names);
  }
  codegen::EmitRequest request{
      .storage = std::move(package.storage),
      .instr_spans = package.instr_spans,
      .instr_site_names = profile.on_panic == pkg::OnPanic::Backtrace
                              ? std::span<const str::StringPoolId>(site_names)
                              : std::span<const str::StringPoolId>{},
      .strings = &ctx.strings,
      .bag = &ctx.bag,
      .target = ctx.target,
      .emit_entry = !is_lib,
      .freestanding = ctx.freestanding,
      .backtrace = profile.on_panic == pkg::OnPanic::Backtrace,
      .optimize = profile.optimize,
      .overlay = profile.overflow_checks ? &checks : nullptr,
      .kind = codegen::OutputKind::Object,
      .profiler = ctx.profiler,
      .parts = parts,
      .workers = ctx.requested_jobs(),
  };
  base::Result<std::vector<std::vector<u8>>, codegen::EmitError> emitted =
      emit_artifacts_with_backend(ctx.backend, std::move(request));
  if (emitted.is_err()) {
    (void)report_emit_failure(ctx, std::move(emitted).unwrap_err(),
                              codegen::OutputKind::Object, "object");
    return base::make_err(diag::Reported{});
  }
  std::vector<std::vector<u8>> objects = std::move(emitted).unwrap();
  std::vector<std::string> paths;
  paths.reserve(objects.size());
  for (usize at = 0; at < objects.size(); ++at) {
    const std::string name =
        at == 0 ? std::string("main.o") : "main." + std::to_string(at) + ".o";
    const std::string path = scratch.join(name);
    if (write_output(ctx, "object", path, objects[at]).is_err()) {
      return base::make_err(diag::Reported{});
    }
    paths.push_back(path);
  }
  return base::make_ok(std::move(paths));
}

base::Result<void, diag::Reported> emit_package_object(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildProfile profile,
    const std::string& output_path,
    bool is_lib) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-object",
                                           "backend");
  return emit_package_output(ctx, package, profile, output_path, is_lib,
                             codegen::OutputKind::Object, "object");
}

base::Result<void, diag::Reported> emit_package_ir(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildProfile profile,
    const std::string& output_path,
    bool is_lib) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-ir", "backend");
  return emit_package_output(ctx, package, profile, output_path, is_lib,
                             codegen::OutputKind::Text, "ir");
}

// What a write input borrows: the file names and address names it points
// at, kept alive for as long as the input is. Constructed in place so the
// spans never outlive their owners.
struct IrWriteInput {
  std::vector<std::string_view> file_names;
  std::vector<u64> file_hashes;
  std::vector<ir::AddrName> addr_names;
  ir::WriteInput input;

  IrWriteInput(PipelineContext& ctx, lowering::LoweredPackage& package) {
    file_names.resize(ctx.sources.file_count());
    file_hashes.resize(file_names.size());
    for (u32 id = 0; id < file_names.size(); ++id) {
      file_names[id] = ctx.sources.name(id).value_or(std::string_view{});
      // The content hash lets a cache tell two programs under one
      // name apart, and a file whose bytes are gone hashes the empty
      // input rather than claiming no hash was computed.
      const std::string_view bytes =
          ctx.sources.bytes(id).value_or(std::string_view{});
      file_hashes[id] = ir::binary::fnv1a64(std::span<const u8>(
          reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
    }
    addr_names.reserve(package.addr_names.size());
    for (const lowering::LoweredPackage::AddrInfo& entry : package.addr_names) {
      addr_names.push_back(ir::AddrName{entry.addr, entry.name, entry.is_param,
                                        entry.is_capture});
    }
    input = ir::WriteInput{
        .storage = &*package.storage,
        .strings = &ctx.strings,
        .instr_spans = package.instr_spans,
        .files = ir::FileTable{.names = file_names, .hashes = file_hashes},
        .addr_names = addr_names,
        .prelude_functions = package.prelude_functions,
        .width = ctx.target.width,
        .compiler_version = ctx.version,
    };
  }
};

// The alcy IR text view. It runs before the backend is consulted, because
// the form belongs to the pipeline: a build with no code generator still
// writes it.
base::Result<void, diag::Reported> emit_package_ir_text(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildProfile profile,
    const std::string& output_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "dump-ir", "backend");
  (void)profile;
  IrWriteInput write_input(ctx, package);
  const std::string text = ir::write_text(write_input.input);
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(text.data()),
                                  text.size());
  return write_output(ctx, "ir", output_path, bytes);
}

// The binary form of the same package, for a reader rather than an eye.
base::Result<void, diag::Reported> emit_package_ir_binary(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildProfile profile,
    const std::string& output_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-ir-bc",
                                           "backend");
  (void)profile;
  IrWriteInput write_input(ctx, package);
  const std::vector<u8> bytes = ir::serialize(write_input.input);
  return write_output(ctx, "ir binary", output_path, bytes);
}

base::Result<void, diag::Reported> emit_package_bitcode(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildProfile profile,
    const std::string& output_path,
    bool is_lib) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-bitcode",
                                           "backend");
  return emit_package_output(ctx, package, profile, output_path, is_lib,
                             codegen::OutputKind::Bitcode, "bitcode");
}

base::Result<void, diag::Reported> emit_package_module(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildProfile profile,
    const std::string& output_path,
    bool is_lib) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "emit-wasm",
                                           "backend");
  return emit_package_output(ctx, package, profile, output_path, is_lib,
                             codegen::OutputKind::Module, "module");
}

base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    LinkOptions link,
    std::span<const std::string> object_paths,
    const std::string& exe_path) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "link", "backend");
  // A configured linker replaces the embedded one; the embedded one runs
  // in-process where the build has it and the host's startup inputs are
  // found. The startup probe is one-time and reads the host's files, so
  // it gets its own row in a time trace.
  if (link.driver.empty()) {
    bool ready = false;
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "lld-startup",
                                               "backend");
      ready = embedded_lld_ready();
    }
    if (ready) {
      return link_with_embedded_lld(ctx, link, object_paths, exe_path);
    }
  }
  const std::string driver =
      link.driver.empty() ? "clang" : std::string(link.driver);
  // The driver's own arguments follow the object, where a library is
  // resolved against it, and stop short of the output, which stays the
  // last word.
  std::vector<std::string> argv;
  argv.reserve(6 + link.args.size() + object_paths.size() + 2);
  argv.emplace_back(driver);
  for (const std::string& object_path : object_paths) {
    argv.emplace_back(object_path);
  }
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
    BuildProfile profile,
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
      return emit_package_object(ctx, lowered, profile, output_path, is_lib);
    }
    if (mode == EmitMode::LlvmIr) {
      return emit_package_ir(ctx, lowered, profile, output_path, is_lib);
    }
    if (mode == EmitMode::LlvmBitcode) {
      return emit_package_bitcode(ctx, lowered, profile, output_path, is_lib);
    }
    if (mode == EmitMode::Ir) {
      return emit_package_ir_text(ctx, lowered, profile, output_path);
    }
    if (mode == EmitMode::IrBinary) {
      return emit_package_ir_binary(ctx, lowered, profile, output_path);
    }
    // Executable: a backend that writes objects hands them to the linker;
    // one whose output is a final module writes that module and stops.
    if (!backend_supports(ctx.backend, codegen::OutputKind::Object,
                          ctx.target)) {
      return emit_package_module(ctx, lowered, profile, output_path, is_lib);
    }
    if (ctx.target.is_wasm() && codegen::backend_available(ctx.backend) &&
        !backend_writes_final_module(ctx.backend, ctx.target)) {
      // A wasm executable is one final module, and no wasm linker lives
      // behind this build (ADR-0049 puts it at M3). Handing the object
      // to the host driver only surfaces its own confusing error, so
      // name the ways to get a wasm executable instead.
      const u32 index =
          ctx.bag.emit<i18n::Key::PipelineWasmExecutableNeedsModule>(
              diag::Severity::Error, diag::Stage::Pipeline,
              DiagCode::UnsupportedOutput, codegen::backend_name(ctx.backend));
      (void)index;
      return base::make_err(diag::Reported{});
    }
    io::TempDir scratch = io::TempDir::create_unique("alcy_build_");
    base::Result<std::vector<std::string>, diag::Reported> objects =
        emit_package_objects(ctx, lowered, profile, scratch, is_lib,
                             codegen_parts(lowered));
    if (objects.is_err()) {
      return base::make_err(diag::Reported{});
    }
    const std::vector<std::string> paths = std::move(objects).unwrap();
    return link_executable(ctx, link, paths, output_path);
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
    BuildProfile profile,
    LinkOptions link,
    EmitMode mode,
    const StdSelection& selection) {
  base::Result<source::FileId, source::SourceError> file = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "load", "frontend");
    return ctx.sources.load(target);
  }();
  if (file.is_err()) {
    const source::SourceError reason = std::move(file).unwrap_err();
    if (reason == source::SourceError::NotRegular) {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineSourceNotRegular>(
          diag::Severity::Error, diag::Stage::Pipeline,
          DiagCode::SourceNotRegular, target);
      (void)index;
    } else {
      const u32 index = ctx.bag.emit<i18n::Key::PipelineCannotRead>(
          diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
          target);
      (void)index;
    }
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
  return build_single_root(ctx, root, output_path, profile, link, mode,
                           selection);
}

base::Result<std::string, diag::Reported> build_single_root(
    PipelineContext& ctx,
    source::FileId root,
    std::string_view output,
    BuildProfile profile,
    LinkOptions link,
    EmitMode mode,
    const StdSelection& selection) {
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      front_end_root(ctx, root, selection);
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  base::Result<lowering::LoweredPackage, diag::Reported> package =
      compile_tree(ctx, std::move(tree).unwrap(), profile.optimize,
                   mode == EmitMode::Executable,
                   ctx.sources.name(root).value_or(std::string_view{}));
  if (package.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Reported{});
  }
  lowering::LoweredPackage lowered = std::move(package).unwrap();
  return emit_output(ctx, lowered, profile, link, mode, std::string(output),
                     false);
}

base::Result<std::string, diag::Reported> build_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    std::string_view output,
    BuildProfile profile,
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
    // A binary that will be linked needs its entry point before the
    // linker sees it.
    if (target_mode == EmitMode::Executable &&
        !check_entry_point(ctx, target)) {
      return base::make_err(diag::Reported{});
    }
    std::string output_path;
    if (output.empty()) {
      // Everything a build writes goes to the out/ the scaffold's own
      // `.gitignore` names, whatever the mode: the package's own, or
      // the member's directory under its suite's. An object or a
      // module beside the manifest landed outside the one region the
      // compiler told git to ignore, so `git status` reported the
      // build's own output as untracked.
      const path::Path out_dir = target.output_dir.has_value()
                                     ? *target.output_dir
                                     : root.join(path::DEFAULT_OUT_DIR);
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
        compile_tree(
            ctx, target.tree, profile.optimize,
            !target.is_lib && planned_target.mode == EmitMode::Executable,
            target.name);
    if (package.is_err() || ctx.bag.has_errors()) {
      failed = true;
      break;
    }
    lowering::LoweredPackage lowered = std::move(package).unwrap();
    base::Result<std::string, diag::Reported> written =
        emit_output(ctx, lowered, profile, link, planned_target.mode,
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

base::Result<std::string, diag::Reported> build_suite(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    std::string_view output,
    BuildProfile profile,
    LinkOptions link,
    EmitMode mode) {
  const std::optional<std::string_view> bytes =
      ctx.sources.bytes(manifest_file);
  if (!bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineManifestNotLoaded>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<pkg::SuiteManifest, diag::Reported> parsed =
      pkg::parse_suite_manifest(*bytes, manifest_name, manifest_file, ctx.bag,
                                ctx.arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const pkg::SuiteManifest suite = std::move(parsed).unwrap();
  base::Result<void, pkg::SuiteError> verified =
      pkg::verify_suite_manifest(suite);
  if (verified.is_err()) {
    pkg::report_suite_error(std::move(verified).unwrap_err(), manifest_name,
                            ctx.bag);
    return base::make_err(diag::Reported{});
  }
  if (suite.package_count == 0) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineSuiteEmpty>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets,
        manifest_name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (!output.empty()) {
    // One `-o` cannot name every member's artifact.
    const u32 index =
        ctx.bag.emit<i18n::Key::PipelineMultipleTargetsWithOutput>(
            diag::Severity::Error, diag::Stage::Pipeline, DiagCode::NoTargets,
            output);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  for (u32 i = 0; i < suite.package_count; ++i) {
    base::Result<ManifestProbe, diag::Reported> probe =
        require_package_manifest(ctx, root.join(suite.packages[i]).as_view(),
                                 false);
    if (probe.is_err()) {
      return base::make_err(diag::Reported{});
    }
    ManifestProbe member = std::move(probe).unwrap();
    base::Result<std::string, diag::Reported> built =
        build_package(ctx, member.root, member.manifest, member.manifest_name,
                      {}, profile, link, mode);
    if (built.is_err()) {
      return base::make_err(diag::Reported{});
    }
  }
  // A member is not more the build than its siblings, so the suite
  // reports the one directory its artifacts share.
  return base::make_ok(std::string(root.join(path::DEFAULT_OUT_DIR).as_view()));
}

}  // namespace pipeline

