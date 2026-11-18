// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"

namespace pipeline {

// Returns the selected members of the embedded standard library as the
// prelude inputs: one facade per member plus the modules beside it. Only
// selected members are returned, so a program sees exactly what its
// manifest names. Each is a virtual source named by its path within the
// suite, so nothing is written to disk and two compilations cannot read
// each other's prelude. Kept for the command and restaged when a later
// call selects different members.
std::span<const analyzer::ModuleInput> std_prelude(
    PipelineContext& ctx,
    const StdSelection& selection);

// The staged source a file is, as the suite holds it.
struct StagedMember {
  // The path within the suite, which is also the module's name there.
  std::string_view path;
  // Whether the source is its package's facade.
  bool is_facade = false;
};

// The staged source `path` is, or nothing when it is none. A checkout
// keeps a member at `lib/std/<path>`, so a path ending in a staged
// source's path on a segment boundary is that member as the suite holds
// it, and checking it by that name checks the member rather than a file
// that merely resembles it.
std::optional<StagedMember> staged_member(std::string_view path,
                                          const StdSelection& selection);

// The spec policy rows of the selected members, each in the
// embedded suite and carrying the seals its manifest declares. A
// package build reads these from the loaded manifests; a single-file
// compilation has none, so the rows come from the embedded table.
std::vector<analyzer::PackagePolicy> std_package_policies(
    const StdSelection& selection);

}  // namespace pipeline
