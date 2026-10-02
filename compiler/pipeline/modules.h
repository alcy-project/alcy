// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

// Selects the modules a manifest declares: explicit `include` entries map
// to `<entry>.al` under `root`, while a wildcard adds every discovered
// file by its root-relative path. An include that matches no file is an
// error, and a discovered file that no entry selected is a warning.
//
// This reads the manifest rather than the filesystem - the files were
// discovered and loaded before this - so it lives beside discovery, in the
// module that knows what the run found. Names are arena storage in `ctx`,
// because `analyzer::ModuleInput` holds its name as a view, and the
// facade flag is the prelude's: a package module is never one.
//
// The returned names are not yet target-relative. A target strips its own
// directory from them and names the entry file "".
base::Result<std::vector<analyzer::ModuleInput>, diag::Reported> select_modules(
    PipelineContext& ctx,
    const pkg::PackageManifest& manifest,
    const path::Path& root,
    std::span<const source::FileId> files);

}  // namespace pipeline
