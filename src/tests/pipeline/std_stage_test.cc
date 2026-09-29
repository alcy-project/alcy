// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/std_stage.h"

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"

namespace pipeline {

namespace {

// The staged standard library lives in a scratch directory whose files
// are then memory-mapped, so two contexts sharing one directory corrupt
// each other: the second wipes the directory on construction and rewrites
// each file with "wb", which truncates the inode the first still has
// mapped. Reading past the end of a truncated mapping is a fault, not a
// wrong answer, so the property is stated on the directory rather than on
// the symptom, which needs a race to reproduce.
base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> stage(
    PipelineContext& ctx) {
  return std_prelude(ctx, full_std_selection());
}

// Staging only records the directory on success, so this hands back null
// rather than a reference the caller has no way to check.
const io::TempDir* scratch_of(const PipelineContext& ctx) {
  return ctx.std_scratch.has_value() ? &*ctx.std_scratch : nullptr;
}

}  // namespace

TEST_CASE("Staging the standard library uses a private directory") {
  PipelineContext a;
  PipelineContext b;
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> first =
      stage(a);
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> second =
      stage(b);
  CHECK(first.is_ok());
  CHECK(second.is_ok());
  const io::TempDir* const a_dir = scratch_of(a);
  const io::TempDir* const b_dir = scratch_of(b);
  CHECK(a_dir != nullptr);
  CHECK(b_dir != nullptr);
  if (first.is_err() || second.is_err() || a_dir == nullptr ||
      b_dir == nullptr) {
    return;
  }
  // A shared name would make a parallel build or two editor
  // integrations read each other's prelude.
  CHECK(a_dir->path() != b_dir->path());
}

TEST_CASE("Staging the standard library twice reuses one directory") {
  // Within a context the staged inputs are kept, so the paths handed to
  // the resolver must survive a second call. Staging again under a fresh
  // name would invalidate every view already taken.
  PipelineContext ctx;
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> first =
      stage(ctx);
  CHECK(first.is_ok());
  const io::TempDir* const dir = scratch_of(ctx);
  CHECK(dir != nullptr);
  if (first.is_err() || dir == nullptr) {
    return;
  }
  const std::string_view first_path = dir->path();
  const usize first_count = std::move(first).unwrap().size();
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> second =
      stage(ctx);
  CHECK(second.is_ok());
  const io::TempDir* const again = scratch_of(ctx);
  CHECK(again != nullptr);
  if (second.is_err() || again == nullptr) {
    return;
  }
  CHECK(again->path() == first_path);
  CHECK(std::move(second).unwrap().size() == first_count);
}

TEST_CASE("Staging the standard library gives every context its own path") {
  // Same property as above over several contexts, so a name derived from
  // anything but a per-context random suffix fails here too.
  std::set<std::string> paths;
  for (i32 i = 0; i < 4; ++i) {
    PipelineContext ctx;
    base::Result<std::span<const analyzer::ModuleInput>, diag::Reported>
        staged = stage(ctx);
    const io::TempDir* const dir = scratch_of(ctx);
    CHECK(staged.is_ok());
    CHECK(dir != nullptr);
    if (staged.is_err() || dir == nullptr) {
      continue;
    }
    CHECK(paths.insert(std::string(dir->path())).second);
  }
  CHECK(paths.size() == 4);
}

}  // namespace pipeline
