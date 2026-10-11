// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <vector>

#include "diag/span.h"
#include "fpag/str/string_pool_id.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

// Rendered `file:line:column` spellings parallel to `spans`, interned in
// the context's string table for a backend that names call sites. A span
// that names no known file renders as the empty name, which a backtrace
// prints as no site at all. Built only for a backtrace emission: every
// other build leaves the table - and the interning - out.
std::vector<str::StringPoolId> render_instr_sites(
    PipelineContext& ctx,
    std::span<const diag::Span> spans);

}  // namespace pipeline
