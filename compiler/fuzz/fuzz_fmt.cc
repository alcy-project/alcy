// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Arbitrary bytes through the formatter, then through it again.
//
// The oracle is the fixed point: whatever the bytes are, a second pass
// over the first pass's output moves nothing. That is the property the
// corpus gate asks of every source in the tree; here it is asked of
// arbitrary bytes, which reach the layouts the corpus never spells --
// an unterminated delimiter, a stray closer, bytes the lexer reads as
// errors, all of which the formatter has to lay out rather than
// reject, since the file a formatter is most wanted for is the one
// being edited.
//
// The width is injected rather than fixed, so the pass the mutation
// drives also covers the parameter a host controls and the wrapping
// decisions it changes.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/formatter.h"
#include "fpag/base/numeric.h"

extern "C" i32 LLVMFuzzerTestOneInput(const u8* data, usize size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);

  // A wide width and a narrow one, so the wrapping decisions the input
  // drives differ between the two configurations the property is asked
  // over.
  alcy_fmt::FormatConfig narrow;
  narrow.line_length = 24;
  narrow.indent_width = 1;
  const std::vector<alcy_fmt::FormatConfig> configs{
      alcy_fmt::default_format_config(), narrow};
  for (const alcy_fmt::FormatConfig& config : configs) {
    const std::string once = alcy_fmt::format(bytes, config);
    const std::string twice = alcy_fmt::format(once, config);
    if (getenv("FUZZ_FMT_DEBUG") != nullptr) {
      FILE* out = fopen("/tmp/fmt_dbg_once.bin", "wb");
      if (out != nullptr) {
        fwrite(once.data(), 1, once.size(), out);
        fclose(out);
      }
      out = fopen("/tmp/fmt_dbg_twice.bin", "wb");
      if (out != nullptr) {
        fwrite(twice.data(), 1, twice.size(), out);
        fclose(out);
      }
    }
    if (once != twice) {
      // A layout a second pass moves is a review nobody asked for, and
      // libFuzzer turns the abort into a minimizing reproducer.
      __builtin_trap();
    }
  }
  return 0;
}
