// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/backend.h"

#include <optional>
#include <string>

#include "codegen/target.h"
#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "ir/type.h"
#include "pipeline/backend_emit.h"
#include "pipeline/build.h"
#include "pipeline/diag_code.h"
#include "pipeline/emit_mode.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/profile.h"
#include "pipeline/std_select.h"

namespace codegen {

TEST_CASE("Backend names round-trip") {
  for (const Backend backend : {Backend::Llvm, Backend::DirectWasm}) {
    const std::optional<Backend> found =
        backend_from_name(backend_name(backend));
    CHECK(found.has_value());
    if (found.has_value()) {
      CHECK(*found == backend);
    }
  }
  CHECK(!backend_from_name("native").has_value());
  CHECK(backend_name(Backend::None) == "none");
  CHECK(!backend_available(Backend::None));
}

TEST_CASE("Target names resolve") {
  const std::optional<Target> host = target_from_name("host");
  CHECK(host.has_value());
  if (host.has_value()) {
    CHECK(host->triple == host_triple());
    // The Emscripten build runs on the wasm machine it emits for; every
    // other host is not one.
#if BUILD_FLAG(IS_OS_ASMJS)
    CHECK(host->is_wasm());
#else
    CHECK(!host->is_wasm());
#endif
  }

  const std::optional<Target> wasm =
      target_from_name("wasm32-unknown-emscripten");
  CHECK(wasm.has_value());
  if (wasm.has_value()) {
    CHECK(wasm->is_wasm());
    CHECK(wasm->width == ir::PointerWidth::W32);
  }

  CHECK(!target_from_name("x86_64-unknown-linux-gnu").has_value());
  for (const std::string& name : target_names()) {
    CHECK(target_from_name(name).has_value());
  }
}

TEST_CASE("The default backend follows the target") {
  const Target wasm{"wasm32-unknown-emscripten", ir::PointerWidth::W32};
#if ALCY_BACKEND_LLVM
#if BUILD_FLAG(IS_OS_ASMJS)
  // The host itself is the wasm machine here, and a wasm machine's
  // default backend is the direct one.
  CHECK(default_backend(host_target()) == Backend::DirectWasm);
#else
  CHECK(default_backend(host_target()) == Backend::Llvm);
#endif
#endif
  CHECK(default_backend(wasm) == Backend::DirectWasm);
}

}  // namespace codegen

namespace pipeline {

TEST_CASE("A mode names the output kind a backend answers") {
  const codegen::Target wasm{"wasm32-unknown-emscripten",
                             ir::PointerWidth::W32};
  // Explicit targets: the expectations name the mapping, and the host
  // itself is a wasm machine in the emscripten test build.
  const codegen::Target native{"x86_64-unknown-linux-gnu",
                               ir::PointerWidth::W64};
  // A wasm executable is the final module itself; everywhere else the
  // executable is an object the linker finishes.
  CHECK(output_kind_for(EmitMode::Executable, wasm) ==
        codegen::OutputKind::Module);
  CHECK(output_kind_for(EmitMode::Executable, native) ==
        codegen::OutputKind::Object);
  CHECK(output_kind_for(EmitMode::Object, native) ==
        codegen::OutputKind::Object);
  CHECK(output_kind_for(EmitMode::LlvmIr, native) == codegen::OutputKind::Text);
  CHECK(output_kind_for(EmitMode::LlvmBitcode, native) ==
        codegen::OutputKind::Bitcode);
}

TEST_CASE("The default backend answers what the request asks for") {
  const codegen::Target wasm{"wasm32-unknown-emscripten",
                             ir::PointerWidth::W32};
#if ALCY_BACKEND_LLVM
  // The direct backend is a wasm target's preference, but it only
  // writes a final module; anything else has to reach LLVM.
  CHECK(select_backend(wasm, codegen::OutputKind::Module, false) ==
        codegen::Backend::DirectWasm);
  CHECK(select_backend(wasm, codegen::OutputKind::Text, false) ==
        codegen::Backend::Llvm);
  CHECK(select_backend(wasm, codegen::OutputKind::Bitcode, false) ==
        codegen::Backend::Llvm);
  CHECK(select_backend(wasm, codegen::OutputKind::Object, false) ==
        codegen::Backend::Llvm);
  // No optimizer lives behind the direct backend, so a release request
  // asks the implementation that has one.
  CHECK(select_backend(wasm, codegen::OutputKind::Object, true) ==
        codegen::Backend::Llvm);
  CHECK(select_backend(wasm, codegen::OutputKind::Text, true) ==
        codegen::Backend::Llvm);
  // Neither can optimize a final wasm module; the target default stands
  // so the diagnostic names the backend that was actually chosen.
  CHECK(select_backend(wasm, codegen::OutputKind::Module, true) ==
        codegen::Backend::DirectWasm);
#else
  CHECK(select_backend(wasm, codegen::OutputKind::Module, false) ==
        codegen::Backend::DirectWasm);
  // Nothing in this build writes the other kinds; the target default is
  // still the backend a refusal names.
  CHECK(select_backend(wasm, codegen::OutputKind::Text, false) ==
        codegen::Backend::DirectWasm);
#endif
  CHECK(backend_writes_final_module(codegen::Backend::DirectWasm, wasm));
  // The host is a wasm machine in the emscripten test build, so the
  // native expectation names a native target.
  const codegen::Target native{"x86_64-unknown-linux-gnu",
                               ir::PointerWidth::W64};
  CHECK(!backend_writes_final_module(codegen::Backend::DirectWasm, native));
#if ALCY_BACKEND_LLVM
  CHECK(!backend_writes_final_module(codegen::Backend::Llvm, wasm));
#endif
}

#if ALCY_BACKEND_LLVM
TEST_CASE("A wasm llvm-ir request reaches LLVM") {
  // The direct backend is a wasm target's default, but it writes only a
  // final module; a textual-IR request used to be sent to it and
  // refused. The mode has to pick the implementation that can answer.
  io::TempDir dir = io::TempDir::create_unique("alcy_wasm_ir_test_");
  CHECK(dir.write_file("main.al", "fn main() -> i32 {\n  ret 0\n}\n"));

  const codegen::Target wasm{"wasm32-unknown-emscripten",
                             ir::PointerWidth::W32};
  PipelineContext ctx{i18n::Language::EnUs};
  // What cli_main asks for when the command names no backend.
  ctx.backend =
      select_backend(wasm, output_kind_for(EmitMode::LlvmIr, wasm), false);
  CHECK(ctx.backend == codegen::Backend::Llvm);
  ctx.target = wasm;
  const std::string ir_path = dir.join("main.ll");
  const base::Result<std::string, diag::Reported> built =
      build_single_file(ctx, dir.join("main.al"), ir_path, BuildProfile{},
                        LinkOptions{}, EmitMode::LlvmIr, StdSelection{});
  CHECK(built.is_ok());
  if (built.is_err()) {
    return;
  }
  const std::optional<std::string> text = io::read_file(ir_path);
  CHECK(text.has_value());
  if (text.has_value()) {
    CHECK(text->find("define") != std::string::npos);
  }
}

TEST_CASE("A wasm executable from an object backend is refused") {
  // The host driver is no wasm linker, so `--backend=llvm --target=wasm
  // --emit=executable` must not reach it. The refusal names the two
  // spellings that do work instead of leaving clang's own error.
  io::TempDir dir = io::TempDir::create_unique("alcy_wasm_exe_test_");
  CHECK(dir.write_file("main.al", "fn main() -> i32 {\n  ret 0\n}\n"));

  PipelineContext ctx{i18n::Language::EnUs};
  ctx.backend = codegen::Backend::Llvm;
  ctx.target =
      codegen::Target{"wasm32-unknown-emscripten", ir::PointerWidth::W32};
  const base::Result<std::string, diag::Reported> built = build_single_file(
      ctx, dir.join("main.al"), dir.join("main.wasm"), BuildProfile{},
      LinkOptions{}, EmitMode::Executable, StdSelection{});
  CHECK(built.is_err());
  CHECK(ctx.bag.has_errors());
  const diag::Diagnostic* const only = ctx.bag.at(0);
  CHECK(only != nullptr);
  if (only != nullptr) {
    CHECK(only->code ==
          diag::Code{diag::Stage::Pipeline,
                     static_cast<u8>(DiagCode::UnsupportedOutput)});
    CHECK(only->message.find("--emit=object") != std::string::npos);
    CHECK(only->message.find("--backend=direct-wasm") != std::string::npos);
  }
}
#endif  // ALCY_BACKEND_LLVM

}  // namespace pipeline
