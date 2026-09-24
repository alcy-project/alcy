// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/std_stage.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/io/temp_dir.h"
#include "pipeline/embedded_std.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

namespace {

std::string_view as_view(const unsigned char* data, u64 len) {
  return std::string_view(reinterpret_cast<const char*>(data), len);
}

}  // namespace

std::span<const analyzer::ModuleInput> std_prelude(PipelineContext& ctx) {
  if (ctx.std_staged) {
    return ctx.std_inputs;
  }
  ctx.std_staged = true;
  ctx.std_scratch.emplace("alcy_std");
  io::TempDir& scratch = *ctx.std_scratch;
  if (!scratch.write_file(std_core_name(),
                          as_view(kStdCoreMain, kStdCoreMain_len))) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot stage the standard library");
    (void)index;
    return {};
  }
  base::Result<source::FileId, source::SourceError> loaded =
      ctx.sources.load(scratch.join(std_core_name()));
  if (loaded.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, kPipelineIoError,
                                   "cannot load the standard library");
    (void)index;
    return {};
  }
  ctx.std_inputs.push_back({"core", std::move(loaded).unwrap()});
  return ctx.std_inputs;
}

std::string_view std_core_name() {
  return "core_main.al";
}

}  // namespace pipeline
