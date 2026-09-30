// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/std_stage.h"

#include <optional>
#include <set>
#include <span>
#include <string_view>

#include "analyzer/resolve.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace pipeline {

namespace {

std::span<const analyzer::ModuleInput> stage(PipelineContext& ctx) {
  return std_prelude(ctx, full_std_selection());
}

}  // namespace

// The suite's bytes are copied into the source manager rather than
// written out and mapped, so nothing reaches the filesystem and two
// compilations cannot read each other's prelude. The name a source
// carries is what diagnostics see, and it stays the path within the
// suite: a name carrying a scratch directory would put a temporary path
// in every diagnostic about a standard library source.
TEST_CASE("Staged sources are virtual and named by their suite path") {
  PipelineContext ctx;
  const std::span<const analyzer::ModuleInput> inputs = stage(ctx);
  CHECK(!inputs.empty());
  for (const analyzer::ModuleInput& input : inputs) {
    CHECK(input.id != source::UNKNOWN_FILE);
    CHECK(!ctx.sources.is_mapped(input.id));
    const std::optional<std::string_view> name = ctx.sources.name(input.id);
    CHECK(name.has_value());
    if (!name.has_value()) {
      continue;
    }
    CHECK(*name == input.name);
    CHECK(!name->starts_with("/"));
    const std::optional<std::string_view> bytes = ctx.sources.bytes(input.id);
    CHECK(bytes.has_value());
    if (bytes.has_value()) {
      CHECK(!bytes->empty());
    }
  }
}

TEST_CASE("Staging the standard library twice reuses one source per name") {
  // Within a context the sources are kept, so the ids handed to the
  // resolver must survive a second call. Re-adding a name would mint a
  // second id for one name and leave every view taken from the first
  // reading bytes nothing else refers to.
  PipelineContext ctx;
  const std::span<const analyzer::ModuleInput> first = stage(ctx);
  CHECK(!first.empty());
  if (first.empty()) {
    return;
  }
  const std::optional<std::string_view> text = ctx.sources.bytes(first[0].id);

  const std::span<const analyzer::ModuleInput> second = stage(ctx);
  CHECK(second.size() == first.size());
  for (usize i = 0; i < second.size() && i < first.size(); ++i) {
    CHECK(second[i].id == first[i].id);
    CHECK(ctx.sources.bytes(second[i].id).has_value());
  }
  CHECK(ctx.sources.bytes(first[0].id) == text);
}

TEST_CASE("Staging names one facade per member") {
  // A member's entry module is what puts its public surface in scope
  // without a `use`, so the flag has to follow the name and only the
  // name: a `prelude` module in a user package is ordinary.
  PipelineContext ctx;
  const std::span<const analyzer::ModuleInput> inputs = stage(ctx);
  CHECK(!inputs.empty());
  std::set<std::string_view> names;
  std::set<std::string_view> facades;
  for (const analyzer::ModuleInput& input : inputs) {
    CHECK(names.insert(input.name).second);
    const bool entry_module = input.name.ends_with("/prelude.al");
    CHECK(input.is_facade == entry_module);
    if (input.is_facade) {
      CHECK(facades.insert(input.name).second);
    }
  }
  CHECK(!facades.empty());
}

}  // namespace pipeline
