// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace llvm {
class Module;
}  // namespace llvm

namespace codegen_llvm {

enum class ObjectEmitError : u8 {
  UnknownTriple,
  NoTargetMachine,
  CannotEmit,
  IoError,
};

// Emits a relocatable object file for the module. An empty triple
// selects the host. Only targets linked into the binary are
// available (host backends on native builds); anything else reports
// UnknownTriple without touching the module.
//
// The module arrives as it will be emitted: optimization is
// optimize_module's job, run by the caller beforehand, so that every
// consumer of a module sees the same one.
base::Result<std::vector<u8>, ObjectEmitError> emit_object(
    llvm::Module& module,
    std::string_view triple);

// Runs the O3 middle-end pipeline over the module in place, for the
// given triple (empty selects the host). The module must verify clean;
// the pipeline preserves that. Unknown triples fail the same way
// emission does.
base::Result<void, ObjectEmitError> optimize_module(llvm::Module& module,
                                                    std::string_view triple);

// The module as LLVM's textual IR. This is the only output that can be
// read, diffed, and checked by a tool outside this compiler: an object
// file is written straight from a TargetMachine, so nothing but the
// compiler itself can say what is in it. It also stops before code
// generation, so it is the one output that does not need a target.
//
// Infallible: printing a module to a string cannot fail.
std::string emit_ir(llvm::Module& module);

// The module as LLVM's bitcode: the same content emit_ir prints, written
// in the binary form that `llvm-dis` and a linker read directly instead of
// parsing text first. Like emit_ir it stops before code generation, so it
// needs no target.
//
// Infallible: it is written to memory, and nothing there can fail.
std::vector<u8> emit_bitcode(llvm::Module& module);

}  // namespace codegen_llvm
