// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "pipeline/pipeline_context.h"

namespace linter {

// One repair, as the bytes to put where a span is. The replacement is
// empty to delete the span. Owned, because the pass that produced it is
// gone before a caller writes it.
struct TextEdit {
  u32 offset = 0;
  u32 length = 0;
  std::string replacement;
};

// The repairs of one file, in the order the diagnostics emitted them.
struct FileEdits {
  std::string path;
  std::vector<TextEdit> edits;
};

// The rewritten text and how many edits landed. An edit that overlaps
// one already applied, or names bytes outside the file, is skipped
// rather than half-applied.
struct EditOutcome {
  std::string text;
  usize applied = 0;
};

// The repairs a finished pass carries, grouped by the file their span
// names. A repair whose file the source manager cannot name is left out:
// a client can only write a file it can reopen.
std::vector<FileEdits> collect_fixes(const pipeline::PipelineContext& ctx);

// Applies edits to one file's bytes. The edits need not be sorted; the
// result preserves the order the file had around them.
EditOutcome apply_edits(std::string_view original,
                        std::span<const TextEdit> edits);

}  // namespace linter
