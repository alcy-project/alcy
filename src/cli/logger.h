// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"

namespace cli {

// Finished blocks of output, written to a destination resolved once.
//
// The block contract: non-empty, and ending in exactly one newline. A
// producer owns that, because a diagnostic renders its own lines and the
// help text renders its own, so there is nothing here to add or to strip.
// This checks it and writes the block verbatim, which means a producer
// that forgets is a failing debug build rather than a repair pass over
// text nobody looked at.
//
// The JSON document is built by its own renderer and reaches a
// destination the same way, as one block, but it is not a diagnostic
// block and nothing here reads it.
//
// This holds a destination and no knowledge of what goes to it: not the
// severity, not the colour, not the format. Formatting belongs to
// `diag::render`, and a writer that carried the styling would be a
// second place to change it.
// Whether `text` is a finished block: non-empty, and ending in exactly
// one newline. `block` checks this, and a test checks it of a producer's
// output, so a producer that drifts fails a test rather than a run.
constexpr bool is_block(std::string_view text) {
  return !text.empty() && text.back() == '\n' &&
         (text.size() == 1 || text[text.size() - 2] != '\n');
}

class Logger {
 public:
  // Where a block goes, and the write that puts it there. A function
  // pointer and a context rather than a virtual or a std::function: no
  // allocation, no vtable, nothing to tear down.
  using Write = void (*)(void* ctx, std::string_view block);

  constexpr Logger(Write write, void* ctx) noexcept
      : write_(write), ctx_(ctx) {}

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void block(std::string_view text) const;

 private:
  Write write_;
  void* ctx_;
};

// A destination that is a file descriptor. One per stream, owned by the
// caller for as long as a Logger borrows it.
struct FdSink {
  i32 fd;

  static void write(void* ctx, std::string_view block);
};

}  // namespace cli
