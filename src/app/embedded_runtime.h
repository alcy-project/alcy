// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

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
