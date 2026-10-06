// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

// The playground host API: check and compile one source buffer, with no
// filesystem and no command line behind it. A host that embeds the
// compiler -- the browser page first -- calls these three; the CLI is
// not built on them, because a command has more to say than a result
// struct can carry.
//
// The functions have C linkage so a host binds them by name, which is
// what makes them the wasm module's exports. The types are the project's
// numeric aliases, so the header is C++ like the rest of the compiler.
extern "C" {

// Everything one call answers. `wasm` and `diagnostics` are owned by the
// result until `alcy_release`; a null pointer with a zero length means
// the field never applied (a check has no module) or the call failed
// before producing it.
//
// `diagnostics` is the JSON array the CLI's `--json` document carries
// under "diagnostics", so the page and the command line cannot disagree
// about a message. `ok` is 1 when the source was accepted. On wasm32
// every word is four bytes, which is the layout the JS wrapper reads.
typedef struct AlcyResult {
  u8* wasm;
  usize wasm_len;
  u8* diagnostics;
  usize diagnostics_len;
  u32 file_count;
  usize module_count;
  usize function_count;
  i32 ok;
} AlcyResult;

// Checks `src` the way `alcy check` does. The counts describe the
// program; no module is produced. Returns 1 when the source checked and
// 0 when it did not; `out` is always filled.
i32 alcy_check(const u8* src, usize len, AlcyResult* out);

// Compiles `src` to a wasm module for wasm32, the way
// `alcy compile --target=wasm32-unknown-emscripten` does with alcy's own
// emitter. Returns 1 when the module was produced and 0 when it was not;
// `out` is always filled.
i32 alcy_compile(const u8* src, usize len, AlcyResult* out);

// Frees what a call allocated and zeroes the result. Releasing a result
// twice, or one that was never filled, is safe.
void alcy_release(AlcyResult* out);

}  // extern "C"
