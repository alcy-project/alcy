// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "pkg/toolchain.h"
#include "source/source.h"

namespace pipeline {

// Resolved build target: the module tree plus its source count,
// target name, and whether it is a library. Shared by check, build,
// and run; callers report and map failures to their own result codes.
// `name` borrows manifest arena storage through the caller's
// PipelineContext, so it stays valid as long as the context outlives
// the target.
struct PackageTarget {
  analyzer::ModuleTree tree;
  usize file_count = 0;
  std::string_view name;
  bool is_lib = false;
  // Where a default build writes: the suite member's directory under
  // the suite's `out/`, or the package's own `out/` (ADR-0057). Set by
  // resolve_package_targets.
  std::optional<path::Path> output_dir;
};

// The suite a package sits inside: its root, its manifest, and the
// suite-relative path the package's directory has there.
struct EnclosingSuite {
  path::Path root;
  pkg::SuiteManifest manifest;
  std::string member;
};

// `owner/name`, or `name` when the suite declares no owner.
std::string suite_spelling(const pkg::SuiteManifest& suite);

// Whether the suite lists that member path.
bool suite_lists_member(const pkg::SuiteManifest& suite,
                        std::string_view member);

// Walks from the package directory's parent to the filesystem root,
// looking for the nearest manifest. A suite manifest is returned; a
// package manifest ends the walk, because a package is not a suite.
// Diagnostics go to the bag; no value means the package is standalone,
// or that the walk reported an error.
std::optional<EnclosingSuite> find_enclosing_suite(
    PipelineContext& ctx,
    const path::Path& package_dir);

// A manifest probe result: `found` with a loaded manifest, or absent
// when the raw target has no alcy.toml. Path errors are emitted to the
// bag and reported as a failed probe.
struct ManifestProbe {
  bool found = false;
  path::Path root;
  source::FileId manifest = source::UNKNOWN_FILE;
  std::string manifest_name;
};

// Probes a raw cli target for a package manifest and loads it. A target
// without one is reported here, because "build, check, and run all need
// a manifest" is a pipeline fact and three commands writing that message
// three ways is three chances to disagree. `file_hint` selects the
// wording for a command that also accepts a single file with `--file`;
// only `check` does.
base::Result<ManifestProbe, diag::Reported> require_package_manifest(
    PipelineContext& ctx,
    std::string_view raw,
    bool file_hint);

// Which declared targets resolution returns. A run wants the binary
// alone: a library it will not execute is not its business, so one
// the module selection leaves out must not refuse the run.
enum class TargetScope : u8 {
  All,
  BinOnly,
};

// Parses the manifest and resolves the targets `scope` names: the
// binary first when one exists, then the library. Shared by build,
// check, and run; the manifest views borrow the context arena.
base::Result<std::vector<PackageTarget>, diag::Reported>
resolve_package_targets(PipelineContext& ctx,
                        const path::Path& root,
                        source::FileId manifest_file,
                        std::string_view manifest_name,
                        TargetScope scope);

// Loads `.alcy/toolchain.toml` beside the package root. An absent file
// means defaults; a corrupt one lands in the bag.
base::Result<pkg::Toolchain, diag::Reported> load_toolchain(
    PipelineContext& ctx,
    const path::Path& root);

}  // namespace pipeline
