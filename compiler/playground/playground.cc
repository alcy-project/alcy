// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "playground/playground.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "codegen/backend.h"
#include "codegen/target.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/json.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "i18n/messages.h"
#include "pipeline/backend_emit.h"
#include "pipeline/check.h"
#include "pipeline/diag_code.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace {

// The name diagnostics point at: the playground has no file, so every
// span resolves against the name the host gave its buffer.
constexpr std::string_view SOURCE_NAME = "<playground>";

constexpr i18n::Language LANGUAGE = i18n::Language::EnUs;

codegen::Target wasm_target() {
  const std::optional<codegen::Target> target =
      codegen::target_from_name("wasm32-unknown-emscripten");
  DCHECK(target.has_value());
  return target.has_value() ? *target : codegen::host_target();
}

// The context every call runs in: the wasm machine and alcy's own
// emitter, so a native build of this API answers what the wasm build
// does and a host can test either.
void prepare(pipeline::PipelineContext& ctx) {
  ctx.target = wasm_target();
  ctx.backend = codegen::Backend::DirectWasm;
  ctx.profiler = nullptr;
}

u8* copy_bytes(const void* data, usize size) {
  if (size == 0) {
    return nullptr;
  }
  // The host holds these bytes across the C ABI, so no container here
  // can own them; alcy_release is the free.
  // ast-grep-ignore: no-c-heap
  auto* bytes = static_cast<u8*>(std::malloc(size));
  if (bytes == nullptr) {
    return nullptr;
  }
  std::memcpy(bytes, data, size);
  return bytes;
}

void reset(AlcyResult* out) {
  out->wasm = nullptr;
  out->wasm_len = 0;
  out->diagnostics = nullptr;
  out->diagnostics_len = 0;
  out->file_count = 0;
  out->module_count = 0;
  out->function_count = 0;
  out->ok = 0;
}

void fill_diagnostics(pipeline::PipelineContext& ctx, AlcyResult* out) {
  std::string json;
  diag::append_diagnostics_json(json, &ctx.bag, &ctx.sources, LANGUAGE);
  out->diagnostics = copy_bytes(json.data(), json.size());
  out->diagnostics_len = json.size();
}

// The one line the CLI would print when a backend refuses without a
// message of its own: a build without the emitter, or a target it cannot
// write.
void report_emit_failure(pipeline::PipelineContext& ctx) {
  if (ctx.bag.has_errors()) {
    return;
  }
  (void)ctx.bag.emit<i18n::Key::PipelineBackendUnsupported>(
      diag::Severity::Error, diag::Stage::Pipeline,
      pipeline::DiagCode::UnsupportedOutput,
      codegen::backend_name(ctx.backend));
}

}  // namespace

extern "C" i32 alcy_check(const u8* src, usize len, AlcyResult* out) {
  if (out == nullptr) {
    return 0;
  }
  reset(out);
  pipeline::PipelineContext ctx{LANGUAGE};
  prepare(ctx);
  const std::string_view source(reinterpret_cast<const char*>(src), len);
  base::Result<pipeline::CheckOutcome, diag::Reported> result =
      pipeline::check_source(ctx, SOURCE_NAME, source);
  if (result.is_ok()) {
    const pipeline::CheckOutcome counts = std::move(result).unwrap();
    out->file_count = static_cast<u32>(counts.file_count);
    out->module_count = counts.module_count;
    out->function_count = counts.function_count;
    out->ok = 1;
  }
  fill_diagnostics(ctx, out);
  return out->ok;
}

extern "C" i32 alcy_compile(const u8* src, usize len, AlcyResult* out) {
  if (out == nullptr) {
    return 0;
  }
  reset(out);
  pipeline::PipelineContext ctx{LANGUAGE};
  prepare(ctx);
  const std::string_view source(reinterpret_cast<const char*>(src), len);

  const source::FileId root = ctx.sources.add_virtual(SOURCE_NAME, source);
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      pipeline::front_end_root(ctx, root, pipeline::full_std_selection());
  if (tree.is_err() || ctx.bag.has_errors()) {
    fill_diagnostics(ctx, out);
    return 0;
  }
  base::Result<pipeline::FrontendOutput, diag::Reported> front =
      pipeline::run_frontend(ctx, std::move(tree).unwrap());
  if (front.is_err() || ctx.bag.has_errors()) {
    fill_diagnostics(ctx, out);
    return 0;
  }
  pipeline::FrontendOutput package = std::move(front).unwrap();
  out->file_count = 1;
  out->module_count = package.module_count;
  out->function_count = package.function_count;

  codegen::EmitRequest request{
      .storage = std::move(package.package.storage),
      .instr_spans = package.package.instr_spans,
      .strings = &ctx.strings,
      .bag = &ctx.bag,
      .target = ctx.target,
      .emit_entry = true,
      .optimize = false,
      .kind = codegen::OutputKind::Module,
      .profiler = ctx.profiler,
  };
  base::Result<std::vector<u8>, codegen::EmitError> bytes =
      pipeline::emit_with_backend(ctx.backend, std::move(request));
  if (bytes.is_err()) {
    report_emit_failure(ctx);
    fill_diagnostics(ctx, out);
    return 0;
  }
  const std::vector<u8> wasm = std::move(bytes).unwrap();
  out->wasm = copy_bytes(wasm.data(), wasm.size());
  out->wasm_len = wasm.size();
  out->ok = 1;
  fill_diagnostics(ctx, out);
  return out->ok;
}

extern "C" void alcy_release(AlcyResult* out) {
  if (out == nullptr) {
    return;
  }
  // ast-grep-ignore: no-c-heap
  std::free(out->wasm);
  // ast-grep-ignore: no-c-heap
  std::free(out->diagnostics);
  reset(out);
}
