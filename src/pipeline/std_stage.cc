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

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

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
  // The scratch directory must be unique per process. A fixed name is
  // wiped and recreated on construction, and each staged file is then
  // written with "wb", which truncates the existing inode rather than
  // replacing it. Two alcy processes sharing one directory therefore
  // truncate a prelude the other has already mapped, and the reader then
  // faults past the end of a truncated mapping. Parallel builds and
  // concurrent editor integrations hit this routinely.
  ctx.std_scratch.emplace(io::TempDir::create_unique("alcy_std_"));
  io::TempDir& scratch = *ctx.std_scratch;
  // One entry module per package of the `alcy/std` suite, named by its
  // path within the suite. The suite is injected whole until package
  // selection lands; see docs/adr/0016.
  for (usize i = 0; i < STAGED_SOURCE_COUNT; ++i) {
    const StagedSource& source = STAGED_SOURCES[i];
    if (!scratch.write_file(
            std::string_view(source.path),
            as_view(source.data,
                    static_cast<decltype(source.len)>(source.len)))) {
      const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                     "cannot stage the standard library");
      (void)index;
      return base::make_err(diag::Reported{});
    }
    base::Result<source::FileId, source::SourceError> loaded =
        ctx.sources.load(scratch.join(std::string_view(source.path)));
    if (loaded.is_err()) {
      const u32 index = ctx.bag.emit(diag::Severity::Error, PIPELINE_IO_ERROR,
                                     "cannot load the standard library");
      (void)index;
      return base::make_err(diag::Reported{});
    }
    // A source named `prelude.al` is its package's facade: its public
    // surface is in scope without a `use`. The name is read from what was
    // staged, so a `prelude` module in a user package stays ordinary.
    const bool facade = ends_with(source.path, "/prelude.al");
    ctx.std_inputs.push_back(
        {std::string_view(source.path), std::move(loaded).unwrap(), facade});
  }
  ctx.std_staged = true;
  return base::make_ok(std::span<const analyzer::ModuleInput>(ctx.std_inputs));
}

}  // namespace pipeline
