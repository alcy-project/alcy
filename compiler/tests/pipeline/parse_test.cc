// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/parse.h"

#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/page_allocator.h"
#include "i18n/language.h"
#include "pipeline/diag_code.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "tests/util/virtual_source.h"

namespace pipeline {

// A run whose syntax arena is spent refuses the file it was about to
// read, and stops there. The files before it keep their diagnostics and
// the files after it are not reported at all, so a refusal is one
// diagnostic however many files the input has - and it is the same one
// whichever thread read the input first.
TEST_CASE("A parse stops at the first file the arena cannot hold") {
  // A reservation is taken in whole pages, and the page is not 4 KiB on
  // every platform, so the size asked for is the size that can be had.
  const usize capacity = mem::page_size();
  std::deque<std::string> names;
  std::deque<std::string> text;
  tests::DeclaredSources dir;
  dir.add("main.al", "fn main() {}\n");
  for (u32 i = 0; i < 3; ++i) {
    names.push_back("m" + std::to_string(i) + ".al");
    text.push_back("fn f" + std::to_string(i) + "() {}\n");
    dir.add(names.back(), text.back());
  }

  for (u32 jobs : {1u, 8u}) {
    PipelineContext ctx{i18n::Language::EnUs, capacity};
    ctx.jobs = jobs;
    // Past the headroom, which is what the check asks about.
    CHECK(ctx.ast.spans.alloc(capacity - 1) != nullptr);
    CHECK(ctx.ast.nearly_full());

    std::vector<source::FileId> files;
    for (u32 i = 0; i < dir.size(); ++i) {
      const tests::VirtualSource& file = dir.at(i);
      // A parse reads bytes and a file id and nothing else, so the
      // sources go in under their own names with no module name made.
      files.push_back(ctx.sources.add_virtual(file.name, file.bytes));
    }
    base::Result<ParsedFiles, diag::Reported> result = parse_files(ctx, files);
    CHECK(result.is_err());
    CHECK(ctx.bag.size() == 1);
    const diag::Diagnostic* const only = ctx.bag.at(0);
    CHECK(only != nullptr);
    if (only != nullptr) {
      // The refusal is the pipeline's span-arena code, which is what
      // says the input was too large rather than wrong.
      CHECK(only->code ==
            diag::Code{diag::Stage::Pipeline,
                       static_cast<u8>(DiagCode::SpanArenaExhausted)});
    }
  }
}

// Parsing does not know module names, which is what lets one parse serve
// every target of a package: the items come back keyed by file, and a
// caller pairs them with whatever names it has.
TEST_CASE("A parse returns items for the files it was given") {
  PipelineContext ctx{i18n::Language::EnUs};
  const source::FileId root =
      ctx.sources.add_virtual("main.al", "fn main() {}\n");
  const source::FileId util =
      ctx.sources.add_virtual("util.al", "pub fn help() {}\n");
  const source::FileId files[] = {root, util};
  base::Result<ParsedFiles, diag::Reported> result = parse_files(ctx, files);
  CHECK(result.is_ok());
  if (result.is_err()) {
    return;
  }
  const ParsedFiles parsed = std::move(result).unwrap();
  CHECK(parsed.files.size() == 2);
  const ParsedFile* const parsed_root = parsed.find(root);
  const ParsedFile* const parsed_util = parsed.find(util);
  CHECK(parsed_root != nullptr);
  CHECK(parsed_util != nullptr);
  if (parsed_root == nullptr || parsed_util == nullptr) {
    return;
  }
  CHECK(parsed_root->items.size() == 1);
  CHECK(parsed_util->items.size() == 1);
  // The module name is the caller's to assign, so the parse reports the
  // file by its path and no more.
  CHECK(parsed_root->path == "main.al");
  CHECK(parsed_util->path == "util.al");
  // A file that was never an input has nothing under its id.
  CHECK(parsed.find(source::UNKNOWN_FILE) == nullptr);
}

}  // namespace pipeline
