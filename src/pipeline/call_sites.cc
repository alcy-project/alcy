// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/call_sites.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "diag/render.h"
#include "diag/span.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

std::vector<str::StringPoolId> render_instr_sites(
    PipelineContext& ctx,
    std::span<const diag::Span> spans) {
  std::vector<str::StringPoolId> out;
  out.reserve(spans.size());
  for (const diag::Span span : spans) {
    if (span.file == source::UNKNOWN_FILE) {
      out.push_back(str::EMPTY_STRING_ID);
      continue;
    }
    const std::optional<std::string_view> name = ctx.sources.name(span.file);
    const std::optional<std::string_view> bytes = ctx.sources.bytes(span.file);
    if (!name.has_value() || !bytes.has_value()) {
      out.push_back(str::EMPTY_STRING_ID);
      continue;
    }
    const diag::LineColumn at = diag::line_column(*bytes, span.offset);
    const std::optional<str::StringPoolId> id = ctx.strings.intern_copied(
        fmt::format("{}:{}:{}", *name, at.line, at.column));
    out.push_back(id.value_or(str::EMPTY_STRING_ID));
  }
  return out;
}

}  // namespace pipeline
