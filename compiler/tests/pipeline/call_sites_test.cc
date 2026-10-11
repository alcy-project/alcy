// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/call_sites.h"

#include <span>
#include <string_view>
#include <vector>

#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/language.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

TEST_CASE("Call sites render the instruction spans") {
  PipelineContext ctx{i18n::Language::EnUs};
  const source::FileId main =
      ctx.sources.add_virtual("main.al", "fn main() {\n  panic(\"x\")\n}\n");
  const std::vector<diag::Span> spans{
      diag::Span{.file = main, .offset = 14, .length = 5},
      diag::Span{.file = source::UNKNOWN_FILE, .offset = 0, .length = 0},
      diag::Span{.file = 9999, .offset = 0, .length = 0},
  };
  const std::vector<str::StringPoolId> sites = render_instr_sites(ctx, spans);
  CHECK(sites.size() == 3);
  if (sites.size() != 3) {
    return;
  }
  CHECK(ctx.strings.get(sites[0]) == "main.al:2:3");
  CHECK(sites[1] == str::EMPTY_STRING_ID);
  CHECK(sites[2] == str::EMPTY_STRING_ID);
  CHECK(!ctx.bag.has_errors());
}

}  // namespace pipeline
