// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Arbitrary bytes as a source file, through the whole check pipeline.
//
// The broadest target: it reaches the lexer, parser, desugar, resolver,
// checker, lowerer, borrow checker, IR verifier, and emitter. It is also
// the slowest, so the narrower targets exist to reach a stage without
// paying for the ones before it.
//
// The oracle is the pipeline's own. `check_single_file` reports failures
// through the diagnostic bag rather than a return code alone, and the IR
// verifier runs inside lowering, so a bad index in any stage surfaces as
// a diagnostic or as a sanitizer report. What this cannot catch is a
// pipeline that answers wrongly; the property tests and the exe cases
// cover that.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, usize size) {
  // A cap keeps one input from stalling the run: the pipeline is
  // quadratic in places, and a fuzzer wants throughput more than it wants
  // the largest input.
  constexpr usize MAX_SOURCE_BYTES = static_cast<usize>(64) * 1024;
  const usize used = size > MAX_SOURCE_BYTES ? MAX_SOURCE_BYTES : size;
  const std::string_view bytes(reinterpret_cast<const char*>(data), used);

  io::TempDir dir = io::TempDir::create_unique("alcy_fuzz_pipeline_");
  if (!dir.write_file("main.al", bytes)) {
    return 0;
  }
  pipeline::PipelineContext ctx{i18n::Language::EnUs};
  base::Result<pipeline::CheckResult, diag::Reported> result =
      pipeline::check_single_file(ctx, dir.join("main.al"));
  (void)result;
  return 0;
}
