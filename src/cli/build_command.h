// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "cli/result_code.h"

namespace pipeline {

struct PipelineContext;

}  // namespace pipeline

namespace cli {

struct CliConfig;
struct Envelope;

// Builds a package directory.
ResultCode run_build(const CliConfig& config,
                     pipeline::PipelineContext& ctx,
                     Envelope& envelope);

}  // namespace cli
