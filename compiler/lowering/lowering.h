// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profiler.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace lowering {

// AST-to-IR lowering.
//
// Consumes a checked package (AST items plus resolved signatures) and
// produces verifier-ready IR storage. The checked type table is reused
// in place: the package is taken by value and its state reseeds the
// builder, so type indexes stay identical to the analyzer output.
// AST views and spellings borrow the caller's arena and sources;
// `strings` (owned by the cli) interns function names and string
// literal bytes for backend consumption.
//
// Supported input: straight-line functions - literals, locals, struct
// and tuple construction, field access, arithmetic/comparison/cast,
// calls (free, associated, and methods), borrows, blocks, control
// flow, `?`, indexing, enums (including generic instantiations), and
// `const` statics. Anything outside that set diagnoses
// `diag::Stage::Lowering, DiagCode::Unsupported` and fails the lowering (fail
// fast: no dangling references).
//
// Value model (uniform memory, required by codegen's GEP tracking):
// every local and parameter owns an Alloca; aggregates live in memory
// and move through GEP+Load/Store; scalar temporaries are SSA
// registers. Function parameters arrive as entry-block params (lowered
// to PHIs; codegen feeds LLVM args into the entry block) and are
// stored to their allocas on entry. Non-Copy values crossing a move
// position produce a `Move` marker for the borrow checker.
//
// Enum values occupy a slot: an i32 discriminant (the variant index)
// followed by an inline payload area sized and aligned for the widest
// variant. Unit variants store no payload. Matches test the
// discriminant and project a field through the area.
//
// Joins merge through memory (result allocas with per-branch stores),
// never through block parameters, matching what the verifier accepts
// for branch targets.
//
// Runtime hooks (codegen provides the bodies): `print` lowers to
// external `alcy_print(ptr, len)`, `println` to
// `alcy_println(ptr, len)`, and `panic` to `alcy_panic(ptr, len)`
// returning never, followed by `Unreachable`.
//
// Ownership analysis consumes LoweredPackage rather than raw storage:
// instruction spans locate diagnostics, and the address table names
// places.
struct LoweredPackage {
  // Verified when lowering built it; borrow checking and the emitter
  // consume this proof instead of re-verifying.
  ir::VerifiedStorage storage;
  // Parallel to storage instrs by InstructionIdx.
  std::vector<diag::Span> instr_spans;
  // Alloca additions: address, bound name, and whether it backs a
  // parameter (escape analysis treats parameters as external roots)
  // or a borrowed capture (which it treats as derived from outside,
  // so a move through one is refused).
  struct AddrInfo {
    ir::RegisterIdx addr;
    std::string_view name;
    bool is_param = false;
    bool is_capture = false;
  };
  std::vector<AddrInfo> addr_names;
  // Lowered functions from prelude modules. Reported counts exclude
  // them, matching files and modules.
  usize prelude_functions = 0;
};

base::Result<LoweredPackage, diag::Reported> lower_package(
    analyzer::CheckedPackage package,
    ir::PointerWidth width,
    ast::AstArena& ast,
    ir::SymbolTable& strings,
    diag::DiagBag& bag,
    debug::Profiler* profiler = nullptr);

}  // namespace lowering
