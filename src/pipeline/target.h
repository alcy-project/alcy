// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/type.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

// MVP pointer width: isize/usize map to 64-bit integers. An explicit
// choice (never sniffed from the host); a --target flag selects it
// once cross builds land.
constexpr ir::PointerWidth kTargetWidth = ir::PointerWidth::W64;

// Resolved binary target: the module tree plus its source count and
// binary name. Shared by check and build; callers report and map
// failures to their own result codes. `bin_name` borrows manifest arena
// storage through the caller's PipelineContext, so it stays valid as
// long as the context outlives the target.
struct BinTarget {
  analyzer::ModuleTree tree;
  usize file_count = 0;
  std::string_view bin_name;
};

// A manifest probe result: `found` with a loaded manifest, or absent
// when the raw target has no alcy.toml. Path errors are emitted to the
// bag and reported as a failed probe.
struct ManifestProbe {
  bool found = false;
  path::Path root;
  source::FileId manifest = source::kUnknownFile;
  std::string manifest_name;
};

// Probes a raw cli target for a package manifest, loading it when present.
// Shared by build and check so manifest discovery lives in one place.
base::Result<ManifestProbe, path::PathError> find_package_manifest(
    PipelineContext& ctx,
    std::string_view raw);

// Parses the manifest and resolves its single binary target. Shared by
// build and check; the manifest views borrow the context arena.
diag::Fallible<BinTarget> resolve_package_target(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name);

diag::Fallible<BinTarget> resolve_bin_target(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest,
    std::string_view manifest_name);

}  // namespace pipeline
