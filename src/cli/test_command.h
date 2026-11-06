// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace pipeline {

struct PipelineContext;

}  // namespace pipeline

namespace cli {

struct CliConfig;
struct Envelope;

// `alcy test`: build the package under a directory together with the tests
// its modules declare, then run the selected ones and report each as it
// finishes. Returns the process exit status: zero when every selected test
// passed, one when one of them failed, and the shared failure status when
// the package did not build.
i32 run_test(const CliConfig& config,
             pipeline::PipelineContext& ctx,
             Envelope& envelope);

}  // namespace cli
