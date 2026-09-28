// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "pipeline/embedded_std.h"
#include "pkg/manifest.h"

namespace pipeline {

// The members of the embedded suite a build stages, by member name.
// Views borrow the generated tables, so no arena is involved and the
// selection outlives any context.
struct StdSelection {
  std::vector<std::string_view> members;
};

// Resolves manifest dependencies against the embedded `alcy/std`
// suite. A suite glob expands to every member; a bare suite, an
// unknown suite or member, a glob overlapping a member, and a selection
// whose dependency closure is incomplete are all errors. Registry and
// git sources parse but have no fetcher yet, so they are rejected with
// the reason rather than misread. One-segment path entries are left
// alone: they resolve by inclusion through pkg, as before.
base::Result<StdSelection, diag::Reported> resolve_std_selection(
    std::span<const pkg::Dependency> deps,
    diag::DiagBag& bag);

// Every embedded member, for single-file builds and tests: the default
// selection names the whole suite.
inline StdSelection full_std_selection() {
  StdSelection selection;
  for (usize i = 0; i < STD_PACKAGE_COUNT; ++i) {
    selection.members.emplace_back(STD_PACKAGE_DEPS[i].name);
  }
  return selection;
}

// Whether `name` is a public item of any embedded member, naming the
// member. Backs the missing-dependency hint: an unresolved name found
// here is a dependency the manifest does not name.
std::string_view std_symbol_package(std::string_view name);

}  // namespace pipeline
