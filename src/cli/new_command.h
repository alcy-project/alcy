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

// Builds a package directory, filling in what it created.
ResultCode run_new(const CliConfig& config,
                   pipeline::PipelineContext& ctx,
                   Envelope& envelope);

}  // namespace cli
