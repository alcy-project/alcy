// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace base {

// Peak resident set size of this process, in bytes, since the process
// started. It is the high-water mark the kernel kept, so it covers
// everything the invocation touched - argument parsing and the standard
// library included - and not just the compiler's own arenas. Zero when
// the platform offers no way to ask (wasm), which callers read as "not
// measured" rather than as zero bytes.
u64 peak_memory_bytes();

}  // namespace base
