// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace pipeline {

// Embedded program runtime sources (generated). Staged to a scratch
// directory at `alcy build` time so the system compiler can consume
// them without install-layout assumptions.
extern const unsigned char ALCY_RUNTIME_HEADER[];
extern const u64 ALCY_RUNTIME_HEADER_LEN;
extern const unsigned char ALCY_RUNTIME_SOURCE[];
extern const u64 ALCY_RUNTIME_SOURCE_LEN;

}  // namespace pipeline
