// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/std_stage.h"

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
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

base::Result<std::span<const analyzer::ModuleInput>, diag::Reported>
std_prelude(PipelineContext& ctx) {
  if (ctx.std_staged) {
    return base::make_ok(
        std::span<const analyzer::ModuleInput>(ctx.std_inputs));
  }
  ctx.std_scratch.emplace("alcy_std");
  io::TempDir& scratch = *ctx.std_scratch;
  if (!scratch.write_file(std_core_name(),
                          as_view(STD_CORE_MAIN, STD_CORE_MAIN_LEN))) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot stage the standard library");
    (void)index;
    return base::make_err(diag::Reported{});
  }
  base::Result<source::FileId, source::SourceError> loaded =
      ctx.sources.load(scratch.join(std_core_name()));
  if (loaded.is_err()) {
    const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                   "cannot load the standard library");
    (void)index;
    return base::make_err(diag::Reported{});
  }
  ctx.std_inputs.push_back({"core", std::move(loaded).unwrap()});
  ctx.std_staged = true;
  return base::make_ok(std::span<const analyzer::ModuleInput>(ctx.std_inputs));
}

std::string_view std_core_name() {
  return "core_main.al";
}

}  // namespace pipeline
