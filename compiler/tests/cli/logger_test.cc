// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/logger.h"

#include <string>
#include <string_view>
#include <vector>

#include "doctest/doctest.h"

namespace cli {

namespace {

// Collects blocks instead of writing them, which is the whole reason the
// destination is a function pointer and a context.
struct Collector {
  std::vector<std::string> blocks;

  static void write(void* ctx, std::string_view block) {
    static_cast<Collector*>(ctx)->blocks.emplace_back(block);
  }
};

Logger collector(Collector& c) {
  return Logger(&Collector::write, &c);
}

}  // namespace

// A block is non-empty and ends in exactly one newline. Every other
// trailing-newline shape is a producer that has to fix its own text:
// there is nothing to strip or add here, because a repair pass over
// text nobody looked at is how two reports end up with a gap between
// them.
TEST_CASE("A block is one newline at the end and nothing more") {
  CHECK(is_block("\n"));
  CHECK(is_block("error: broken\n"));
  CHECK(is_block("line one\nline two\n"));

  CHECK_FALSE(is_block(""));
  CHECK_FALSE(is_block("no newline at all"));
  CHECK_FALSE(is_block("trailing blank line\n\n"));
  CHECK_FALSE(is_block("\n\n"));

  // Only the end is a contract. A gap between two paragraphs of a help
  // text is the producer's, not the writer's.
  CHECK(is_block("first\n\nthen more\n"));
}

// Concatenation, rather than indexing, so a block that never arrived is
// a mismatch in the text instead of an out-of-range read.
std::string joined(const Collector& c) {
  std::string out;
  for (const std::string& block : c.blocks) {
    out += block;
  }
  return out;
}

TEST_CASE("Blocks reach the destination in order and verbatim") {
  Collector c;
  const Logger log = collector(c);
  log.block("first\n");
  log.block("second\n");
  CHECK(c.blocks.size() == 2);
  CHECK(joined(c) == "first\nsecond\n");
}

TEST_CASE("A block of one empty line is a block") {
  Collector c;
  const Logger log = collector(c);
  log.block("\n");
  CHECK(c.blocks.size() == 1);
  CHECK(joined(c) == "\n");
}

}  // namespace cli
