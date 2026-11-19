// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "cli/logger.h"
#include "diag/render.h"
#include "fpag/base/numeric.h"
#include "pipeline/build.h"

namespace cli {

// What a build announces while it works: one begin line, and for a
// suite one dim line per member that actually reached the compiler.
// Standard error only: the result line on standard output and --json's
// single document stay exactly what they were. A member the build cache
// answered from is not announced, so a fully cached suite prints two
// lines and nothing else.
class BuildProgress {
 public:
  BuildProgress(const Logger& err,
                bool enabled,
                const diag::RenderOptions& options) noexcept
      : err_(err), enabled_(enabled), options_(options) {}

  BuildProgress(const BuildProgress&) = delete;
  BuildProgress& operator=(const BuildProgress&) = delete;

  void begin(std::string_view name, usize count, bool suite) const;
  void member_done(std::string_view member, u64 duration_ns, bool cached) const;

 private:
  const Logger& err_;
  bool enabled_;
  diag::RenderOptions options_;
};

// The plain-callback shape the pipeline accepts, handing it this
// progress object as the context.
pipeline::BuildProgressObserver build_progress_observer(
    const BuildProgress& progress);

}  // namespace cli
