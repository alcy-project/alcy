// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "app/driver_main.h"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "app/driver_config.h"
#include "app/driver_context.h"
#include "app/init_handler.h"
#include "app/new_command.h"
#include "app/parse_args.h"
#include "app/parse_output.h"
#include "app/result_code.h"
#include "app/runtime_stage.h"
#include "app/spawn.h"
#include "base/logger.h"
#include "borrow/borrow.h"
#include "cfg/build_config.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "codegen_llvm/llvm_object_emitter.h"
#include "debug/fatal.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/arg/parser.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/term/color_style.h"
#include "fpag/term/console.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/pipeline.h"
#include "pkg/manifest.h"
#include "pkg/modules.h"
#include "source/source.h"

namespace app {

namespace {

constexpr std::string_view kSourceSuffix = ".al";

// Executable suffix for linked output (Windows needs .exe).
std::string_view exe_suffix() {
#if BUILD_FLAG(IS_OS_WIN)
  return ".exe";
#else
  return "";
#endif
}

i32 build_package(DriverContext& ctx,
                  const path::Path& root,
                  source::FileId manifest_file,
                  std::string_view manifest_name,
                  std::string_view output,
                  bool optimize);

// MVP pointer width: isize/usize map to 64-bit integers. An explicit
// choice (never sniffed from the host); a --target flag selects it
// once cross builds land.
constexpr ir::PointerWidth kCheckWidth = ir::PointerWidth::W64;

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
i32 build_single_file(DriverContext& ctx,
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
    return result_code(ResultCode::BuildFailed);
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      root, {&single_input, 1}, "", ctx.sources, ctx.arena, ctx.bag);
  if (tree.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
  }
  diag::Fallible<lower::LoweredPackage> package =
      compile_tree_to_ir(ctx, std::move(tree).unwrap());
  if (package.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
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
      return result_code(ResultCode::BuildFailed);
    }
    report(ctx.bag, ctx.sources);
    base::logger.wo_prefix("built {} to {}", target, output_path);
    return result_code(ResultCode::Success);
  }
  io::TempDir scratch("alcy_build");
  const std::string object_path = scratch.join("main.o");
  if (!emit_package_object(ctx, lowered, optimize, object_path) ||
      !stage_runtime(scratch)) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
  }
  const std::string runtime_path = scratch.join(runtime_source_name());
  if (!link_executable(ctx, object_path, runtime_path, output_path)) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
  }
  report(ctx.bag, ctx.sources);
  base::logger.wo_prefix("built {} to {}", target, output_path);
  return result_code(ResultCode::Success);
}

i32 run_build(const DriverConfig& config) {
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
    return result_code(ResultCode::BuildFailed);
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
    return result_code(ResultCode::BuildFailed);
  }
  base::logger.wo_prefix("built {} file(s)",
                         std::move(discovered).unwrap().files.size());
  return result_code(ResultCode::Success);
}

i32 check_package(DriverContext& ctx,
                  const path::Path& root,
                  source::FileId manifest_file,
                  std::string_view manifest_name);

// Runs type checking over a resolved tree, reports diagnostics, and
// maps the outcome to an exit code. Shared by manifest and
// single-file modes.
i32 finish_check(DriverContext& ctx,
                 analyzer::ModuleTree tree,
                 usize file_count) {
  diag::Fallible<analyzer::CheckedPackage> checked =
      analyzer::check_package(tree, kCheckWidth, ctx.bag);
  if (checked.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  if (ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  analyzer::CheckedPackage package = std::move(checked).unwrap();
  const usize modules = package.modules.size();
  diag::Fallible<lower::LoweredPackage> lowered = lower::lower_package(
      std::move(package), kCheckWidth, ctx.strings, ctx.bag);
  if (lowered.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  lower::LoweredPackage package_ir = std::move(lowered).unwrap();
  const usize functions = package_ir.storage.functions().size();
  borrow::check_borrows(package_ir, ctx.bag);
  report(ctx.bag, ctx.sources);
  if (ctx.bag.has_errors()) {
    return result_code(ResultCode::CheckFailed);
  }
  base::logger.wo_prefix("checked {} file(s), {} module(s), {} function(s)",
                         file_count, modules, functions);
  return result_code(ResultCode::Success);
}

i32 run_check(const DriverConfig& config) {
  DriverContext ctx;
  const std::string_view raw_target =
      config.target_dir.empty() ? "." : config.target_dir;
  base::Result<path::Path, path::PathError> target =
      path::Path::from_native(raw_target);
  if (target.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "invalid target '{}'", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  const path::Path target_path = std::move(target).unwrap();

  const path::Path manifest_path = target_path.join(pkg::kManifestFileName);
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(manifest_path.as_view());
  if (manifest.is_ok()) {
    return check_package(ctx, target_path, std::move(manifest).unwrap(),
                         manifest_path.as_view());
  }

  // Single-file mode for explicit `foo.al` targets. Directories without
  // a manifest are not checked: module structure needs declared roots.
  if (raw_target.size() < 4 ||
      raw_target.substr(raw_target.size() - 3) != ".al") {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, kDriverNoManifest,
        "no manifest found at '{}'; check a file or add alcy.toml", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(target_path.as_view());
  if (file.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverIoError,
                                   "cannot read '{}'", raw_target);
    (void)index;
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  const source::FileId root = std::move(file).unwrap();
  const analyzer::ModuleInput single_input{"", root};
  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      root, {&single_input, 1}, "", ctx.sources, ctx.arena, ctx.bag);
  if (tree.is_err()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  if (ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  return finish_check(ctx, std::move(tree).unwrap(), 1);
}

// Resolved binary target: the module tree plus its source count and
// binary name. Shared by check and build; callers report and map
// failures to their own result codes.
struct BinTarget {
  analyzer::ModuleTree tree;
  usize file_count = 0;
  std::string_view bin_name;
};

diag::Fallible<BinTarget> resolve_bin_target(DriverContext& ctx,
                                             const path::Path& root,
                                             source::FileId manifest_file,
                                             std::string_view manifest_name) {
  diag::Fallible<pkg::PackageManifest> parsed =
      pkg::parse_manifest(ctx.sources.bytes(manifest_file), manifest_name,
                          manifest_file, ctx.bag, ctx.arena);
  if (parsed.is_err()) {
    return base::make_err(diag::Fatal{});
  }
  const pkg::PackageManifest manifest = std::move(parsed).unwrap();
  if (manifest.bin_count == 0) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverNoTargets,
                                   "manifest '{}' declares no [[bin]] targets",
                                   manifest_name);
    (void)index;
    return base::make_err(diag::Fatal{});
  }
  if (manifest.bin_count > 1) {
    const u32 index = ctx.bag.emit(
        diag::Severity::Error, kDriverNoTargets,
        "manifest '{}' declares {} [[bin]] targets; only one is supported",
        manifest_name, manifest.bin_count);
    (void)index;
    return base::make_err(diag::Fatal{});
  }

  diag::Fallible<pipeline::DiscoveredSources> discovered =
      pipeline::discover_sources(root.as_view(), ctx.sources, ctx.bag);
  if (discovered.is_err()) {
    return base::make_err(diag::Fatal{});
  }
  pipeline::DiscoveredSources found = std::move(discovered).unwrap();
  const std::vector<source::FileId>& files = found.files;
  const path::Path bin_path = root.join(manifest.bins[0].path);
  source::FileId bin_file = source::kUnknownFile;
  for (source::FileId id : files) {
    if (ctx.sources.name(id) == bin_path.as_view()) {
      bin_file = id;
      break;
    }
  }
  if (bin_file == source::kUnknownFile) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverNoTargets,
                                   "bin target '{}' was not discovered",
                                   manifest.bins[0].path);
    (void)index;
    return base::make_err(diag::Fatal{});
  }

  bool bin_selected = false;
  diag::Fallible<std::vector<pkg::ModuleFile>> selection =
      pkg::resolve_module_files(manifest, root.as_view(), files, ctx.sources,
                                ctx.bag, ctx.arena);
  if (selection.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Fatal{});
  }
  std::vector<analyzer::ModuleInput> inputs;
  // Module paths resolve relative to the entry file's directory.
  std::string_view bin_dir;
  {
    const std::string_view bin_path = manifest.bins[0].path;
    const usize slash = bin_path.rfind('/');
    if (slash != std::string_view::npos) {
      bin_dir = bin_path.substr(0, slash);
    }
  }
  for (const pkg::ModuleFile& entry : std::move(selection).unwrap()) {
    if (entry.id == bin_file) {
      bin_selected = true;
      inputs.push_back({"", entry.id});
    } else {
      std::string_view name = entry.name;
      if (!bin_dir.empty() && name.size() > bin_dir.size() &&
          name.substr(0, bin_dir.size()) == bin_dir &&
          name[bin_dir.size()] == '/') {
        name.remove_prefix(bin_dir.size() + 1);
      }
      inputs.push_back({name, entry.id});
    }
  }
  if (!bin_selected) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kDriverNoTargets,
                                   "bin target '{}' is not in [modules]",
                                   manifest.bins[0].path);
    (void)index;
    return base::make_err(diag::Fatal{});
  }

  diag::Fallible<analyzer::ModuleTree> tree = analyzer::resolve_modules(
      bin_file, inputs, manifest.name, ctx.sources, ctx.arena, ctx.bag);
  if (tree.is_err() || ctx.bag.has_errors()) {
    return base::make_err(diag::Fatal{});
  }
  BinTarget target;
  target.tree = std::move(tree).unwrap();
  target.file_count = files.size();
  target.bin_name = manifest.bins[0].name;
  return base::make_ok(target);
}

i32 check_package(DriverContext& ctx,
                  const path::Path& root,
                  source::FileId manifest_file,
                  std::string_view manifest_name) {
  diag::Fallible<BinTarget> target =
      resolve_bin_target(ctx, root, manifest_file, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::CheckFailed);
  }
  BinTarget resolved = std::move(target).unwrap();
  return finish_check(ctx, resolved.tree, resolved.file_count);
}

i32 build_package(DriverContext& ctx,
                  const path::Path& root,
                  source::FileId manifest_file,
                  std::string_view manifest_name,
                  std::string_view output,
                  bool optimize) {
  diag::Fallible<BinTarget> target =
      resolve_bin_target(ctx, root, manifest_file, manifest_name);
  if (target.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
  }
  BinTarget resolved = std::move(target).unwrap();
  diag::Fallible<lower::LoweredPackage> package =
      compile_tree_to_ir(ctx, resolved.tree);
  if (package.is_err() || ctx.bag.has_errors()) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
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
    return result_code(ResultCode::BuildFailed);
  }
  const std::string runtime_path = scratch.join(runtime_source_name());
  if (!link_executable(ctx, object_path, runtime_path, exe_path)) {
    report(ctx.bag, ctx.sources);
    return result_code(ResultCode::BuildFailed);
  }
  report(ctx.bag, ctx.sources);
  base::logger.wo_prefix("built {} to {}", manifest_name, exe_path);
  return result_code(ResultCode::Success);
}

i32 not_implemented(std::string_view subcommand) {
  DriverContext ctx;
  const u32 index =
      ctx.bag.emit(diag::Severity::Error, kDriverNotImplemented,
                   "'alcy {}' is not implemented yet", subcommand);
  (void)index;
  report(ctx.bag, ctx.sources);
  return result_code(ResultCode::NotImplemented);
}

i32 dispatch(const DriverConfig& config) {
  switch (config.subcommand) {
    case Subcommand::Build: return run_build(config);
    case Subcommand::New: return run_new(config.target_dir);
    case Subcommand::Test: return not_implemented("test");
    case Subcommand::Run: return not_implemented("run");
    case Subcommand::Check: return run_check(config);
    case Subcommand::None: break;
  }
  // parse_args maps a missing subcommand to NoSubcommand/UnknownSubcommand,
  // so only interruption outcomes carry Subcommand::None here.
  UNREACHABLE();
}

i32 run_interruption(arg::Parser& parser,
                     const ParseOutcome& outcome,
                     ResultCode code,
                     i32 argc,
                     const char* const* argv) {
  const term::ColorStyle style = term::console_color_style(
      term::Stream::Stdout, scan_color_mode(argc, argv));
  base::init_logger(style);
  const std::string text = render_outcome(parser, outcome, style);
  if (!text.empty()) {
    base::logger.wo_prefix("{}", text);
  }
  return result_code(code);
}

}  // namespace

i32 driver_main(i32 argc, char** argv) {
  init_runtime();

  arg::Parser parser = build_parser();
  ParseOutcome outcome = parse_args(parser, argc, argv);
  if (const std::optional<ResultCode> code = interruption_exit_code(outcome)) {
    return run_interruption(parser, outcome, *code, argc, argv);
  }
  const DriverConfig& config = outcome.get<DriverConfig>();
  base::init_logger(
      term::console_color_style(term::Stream::Stdout, config.color_mode));
  return dispatch(config);
}

}  // namespace app
