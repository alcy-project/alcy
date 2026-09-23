// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "analyzer/resolve.h"
#include "cli/cli_context.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "ir/type.h"
#include "path/path.h"
#include "pkg/manifest.h"

namespace cli {

// MVP pointer width: isize/usize map to 64-bit integers. An explicit
// choice (never sniffed from the host); a --target flag selects it
// once cross builds land.
constexpr ir::PointerWidth kCheckWidth = ir::PointerWidth::W64;

// Resolved binary target: the module tree plus its source count and
// binary name. Shared by check and build; callers report and map
// failures to their own result codes.
struct BinTarget {
  analyzer::ModuleTree tree;
  usize file_count = 0;
  std::string_view bin_name;
};

diag::Fallible<BinTarget> resolve_bin_target(
    CliContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest,
    std::string_view manifest_name);

}  // namespace cli
