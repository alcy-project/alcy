// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace app {

// Embedded program runtime sources (generated). Staged to a scratch
// directory at `alcy build` time so the system compiler can consume
// them without install-layout assumptions.
extern const unsigned char kAlcyRuntimeHeader[];
extern const u64 kAlcyRuntimeHeader_len;
extern const unsigned char kAlcyRuntimeSource[];
extern const u64 kAlcyRuntimeSource_len;

}  // namespace app
