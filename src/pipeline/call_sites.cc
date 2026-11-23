// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/call_sites.h"

#include <algorithm>
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
  std::vector<str::StringPoolId> out(spans.size(), str::EMPTY_STRING_ID);
  // The spans in source order, so one LineCursor answers them in a pass:
  // a position lookup walks from where the last one stopped, and a
  // per-span walk from the top of the text was the bulk of a checked
  // build's site rendering - tens of thousands of spans, each re-counted
  // every line of the file from byte zero.
  std::vector<u32> order;
  order.reserve(spans.size());
  for (u32 at = 0; at < spans.size(); ++at) {
    if (spans[at].file != source::UNKNOWN_FILE) {
      order.push_back(at);
    }
  }
  std::sort(order.begin(), order.end(), [&](u32 left, u32 right) {
    if (spans[left].file != spans[right].file) {
      return spans[left].file < spans[right].file;
    }
    return spans[left].offset < spans[right].offset;
  });
  diag::LineCursor cursor;
  for (const u32 at : order) {
    const diag::Span& span = spans[at];
    const std::optional<std::string_view> name = ctx.sources.name(span.file);
    const std::optional<std::string_view> bytes = ctx.sources.bytes(span.file);
    if (!name.has_value() || !bytes.has_value()) {
      continue;
    }
    const diag::LineColumn position = cursor.locate(*bytes, span.offset);
    const std::optional<str::StringPoolId> id = ctx.strings.intern_copied(
        fmt::format("{}:{}:{}", *name, position.line, position.column));
    out[at] = id.value_or(str::EMPTY_STRING_ID);
  }
  return out;
}

}  // namespace pipeline
