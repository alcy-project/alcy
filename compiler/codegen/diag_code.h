// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace codegen {

// The checks the code generation backends report, counted from 1, in the
// component `Stage::CodegenNative` names. The ids are per stage, so a
// second direct backend shares this table rather than starting its own.
enum class DiagCode : u8 {
  // A construct the backend cannot encode: an instruction, a type, or an
  // operand the emitter has no lowering for. The message and the span
  // name the construct and where it was written.
  Unsupported = 1,
};

}  // namespace codegen
