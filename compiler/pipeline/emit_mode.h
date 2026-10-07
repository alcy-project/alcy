// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace pipeline {

// What a build writes. The mode is chosen explicitly rather than inferred
// from the output's extension, because an extension says what a file is
// called and not what was asked for: `main.o` as a *name* is just as valid
// a request for an executable as `main` is a request for an object.
enum class EmitMode : u8 {
  // Compile, link against the runtime, and write an executable.
  Executable,
  // Write one relocatable object and stop.
  Object,
  // Write the module as LLVM's textual IR and stop. Needs no target, so it
  // is the one mode that works before a backend is chosen.
  LlvmIr,
  // Write the same module as LLVM's bitcode: what the textual mode prints,
  // in the form another LLVM tool reads without parsing text first. Stops
  // before code generation like the textual mode, so it needs no target
  // either.
  LlvmBitcode,
  // Write the lowered package as alcy's own IR text, per
  // `compiler/docs/ir-format.md`. Needs no target and no backend: the
  // form is the pipeline's, not a code generator's.
  Ir,
  // Write the same package in the binary form the same document
  // defines: what a reader rebuilds verified IR from.
  IrBinary,
};

}  // namespace pipeline
