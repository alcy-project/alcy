// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ir/type.h"

namespace llvm {
class Module;
}  // namespace llvm

namespace codegen_llvm {

// Defines the program runtime in `module`: `alcy_print`, `alcy_println`,
// `alcy_panic`, `alcy_sys_write`, `alcy_alloc`, and `alcy_dealloc`,
// along with the internal helper they share. A declaration the program
// already made for one of them is filled in rather than duplicated.
//
// The runtime is defined here rather than compiled from a C source on
// every build, so no system compiler is spawned for it and it joins the
// same optimization pipeline as the program.
//
// Pointer-sized values follow `width`, which must be the width the
// program was lowered with. The module is left without a target: the
// functions use opaque pointers and fix the few element types they
// address, and the emitter sets the triple afterwards.
void add_runtime_definitions(llvm::Module& module, ir::PointerWidth width);

}  // namespace codegen_llvm
