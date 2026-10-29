// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/fix.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "linter/fix.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace linter {

std::vector<FileEdits> collect_fixes(const pipeline::PipelineContext& ctx) {
  std::vector<FileEdits> files;
  ctx.bag.for_each([&](const diag::Diagnostic& diagnostic) {
    if (diagnostic.fix == nullptr) {
      return;
    }
    const std::optional<std::string_view> name =
        ctx.sources.name(diagnostic.fix->span.file);
    if (!name.has_value()) {
      return;
    }
    FileEdits* file = nullptr;
    for (FileEdits& candidate : files) {
      if (candidate.path == *name) {
        file = &candidate;
        break;
      }
    }
    if (file == nullptr) {
      files.push_back(FileEdits{.path = std::string(*name), .edits = {}});
      file = &files.back();
    }
    file->edits.push_back(TextEdit{
        .offset = diagnostic.fix->span.offset,
        .length = diagnostic.fix->span.length,
        .replacement = std::string(diagnostic.fix->replacement),
    });
  });
  return files;
}

EditOutcome apply_edits(std::string_view original,
                        std::span<const TextEdit> edits) {
  std::vector<const TextEdit*> ordered;
  ordered.reserve(edits.size());
  for (const TextEdit& edit : edits) {
    ordered.push_back(&edit);
  }
  // Stable, so two repairs at one offset keep the order the rules
  // emitted them in rather than whichever the sort happens to leave
  // first.
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const TextEdit* left, const TextEdit* right) {
                     return left->offset < right->offset;
                   });

  EditOutcome out;
  out.text.reserve(original.size());
  usize cursor = 0;
  for (const TextEdit* edit : ordered) {
    const usize begin = edit->offset;
    if (begin > original.size() || edit->length > original.size() - begin) {
      continue;
    }
    const usize end = begin + edit->length;
    if (begin < cursor) {
      // Another repair already owns these bytes. Applying both would
      // write text neither rule chose, so the later one is dropped and
      // its finding stays for the next pass.
      continue;
    }
    out.text.append(original.substr(cursor, begin - cursor));
    out.text.append(edit->replacement);
    cursor = end;
    ++out.applied;
  }
  out.text.append(original.substr(cursor));
  return out;
}

}  // namespace linter
