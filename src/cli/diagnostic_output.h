// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace diag {

class DiagBag;
struct RenderOptions;

}  // namespace diag

namespace source {

class SourceManager;

}  // namespace source

namespace cli {

// Renders diagnostics through the CLI-owned output path.
void report_diagnostics(const diag::DiagBag& bag,
                        const source::SourceManager& sources,
                        const diag::RenderOptions& options);

}  // namespace cli
