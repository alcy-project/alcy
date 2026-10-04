// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>

#include "diag/bag.h"
#include "fpag/base/result.h"
#include "pipeline/link_options.h"

namespace pipeline {

struct PipelineContext;

// Whether this compiler links in-process with the lld it was built with:
// the build embedded one (see `alcy_embedded_lld` in
// //third_party/llvm/llvm.gni) and the host's startup inputs are where
// the linker expects them. False falls back to the configured driver,
// which is what every build did before the linker was embedded.
bool embedded_lld_ready();

// Links `object_path` into `exe_path` with the embedded lld, passing the
// configured driver arguments through. Only called when
// `embedded_lld_ready()` is true.
base::Result<void, diag::Reported> link_with_embedded_lld(
    PipelineContext& ctx,
    const LinkOptions& link,
    const std::string& object_path,
    const std::string& exe_path);

}  // namespace pipeline
