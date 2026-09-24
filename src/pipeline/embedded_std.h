// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace pipeline {

// Embedded standard library sources (generated). Staged to a scratch
// directory at build/check time and injected as prelude modules, so
// compilations need no install-layout assumptions.
extern const unsigned char kStdCoreMain[];
extern const u64 kStdCoreMain_len;

}  // namespace pipeline
