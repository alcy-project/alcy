// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "diag/render.h"
#include "pipeline/test.h"

namespace cli {

// The line that opens a run: the verb and subject in the same column
// `run` uses for its own announcement. The options carry both the
// language of the wording and the stream's ability to show colour.
std::string render_test_heading(std::string_view subject,
                                const diag::RenderOptions& options);

// One finished line, newline included, for a test that ended. The
// duration rides after the verdict: `format_duration`'s wording, or
// "unmeasured" when the run had no witness of the test's process.
std::string render_test_case(const pipeline::TestEvent& event,
                             const diag::RenderOptions& options);

}  // namespace cli
