// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>

#include "fpag/base/numeric.h"

namespace pipeline {

// One embedded standard library source, named by its suite-relative
// path (`core/prelude.al`). Staged to a scratch directory and injected
// as a prelude module, so compilations need no install-layout
// assumptions. The table is generated; see build/scripts/embed_std.py.
struct StagedSource {
  const char* path;
  const unsigned char* data;
  u64 len;
};

extern const StagedSource STAGED_SOURCES[];
extern const usize STAGED_SOURCE_COUNT;

}  // namespace pipeline
