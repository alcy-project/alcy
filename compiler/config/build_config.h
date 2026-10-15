// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/build/build_config.h"

// Emscripten is the wasm host: no system linker, no process to spawn,
// nothing that needs a symbol from the platform's own object. fpag reads
// the flag off `__wasm__`, which a driver compiling for the wasm target
// does not define where this header is read.
#if defined(__EMSCRIPTEN__) && \
    !defined(FPAG_BUILD_FLAG_INTERNAL_IS_OS_ASMJS_SET)
#undef FPAG_BUILD_FLAG_INTERNAL_IS_OS_ASMJS
#define FPAG_BUILD_FLAG_INTERNAL_IS_OS_ASMJS() (1)
#define FPAG_BUILD_FLAG_INTERNAL_IS_OS_ASMJS_SET 1
#endif

#define BUILD_FLAG(flag) FPAG_BUILD_FLAG(flag)

