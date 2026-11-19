// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "codegen/x86_64/frame.h"
#include "codegen/x86_64/minst.h"

namespace codegen::x86_64 {

// Resolves a selected body to the form the encoder reads. Every vreg
// becomes the frame slot its type sized, every operand that is not
// already a register is brought into one through a scratch, and the
// entry and exit sequences are put in. Values live in slots, so a
// scratch is free again by the next instruction and no register holds a
// value across one.
//
// The result carries no vreg: anything left is an emitter bug, and the
// encoder refuses to read one.
void allocate(Body& body, const Frame& frame);

}  // namespace codegen::x86_64
