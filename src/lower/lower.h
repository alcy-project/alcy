// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/storage.h"

namespace lower {

// AST-to-IR lowering.
//
// Consumes a checked package (AST items plus resolved signatures) and
// produces verifier-ready IR storage. The checked type table is reused
// in place: the package is taken by value and its state reseeds the
// builder, so type indexes stay identical to the analyzer output.
// AST views and spellings borrow the caller's arena and sources;
// `strings` (owned by the driver) interns function names and string
// literal bytes for backend consumption.
//
// Supported input: straight-line functions — literals, locals, struct
// and tuple construction, field access, arithmetic/comparison/cast,
// calls (free, associated, methods, blessed-free), borrows, blocks,
// `ret`. Control flow (`if`/`match`/loops), `?`, indexing, enums,
// and statics diagnose `kLowerUnsupported` and fail the lowering
// (fail fast: no dangling references).
//
// Value model (uniform memory, required by codegen's GEP tracking):
// every local and parameter owns an Alloca; aggregates live in memory
// and move through GEP+Load/Store; scalar temporaries are SSA
// registers. Function parameters arrive as entry-block params (lowered
// to PHIs; codegen feeds LLVM args into the entry block) and are
// stored to their allocas on entry. Non-Copy values crossing a move
// position produce a `Move` marker for the borrow checker.
//
// Runtime hooks (codegen provides the bodies):
// `print` lowers to external `alcy_print(ptr) -> ()` and `panic` to
// external `alcy_panic(ptr) -> !` followed by `Unreachable`.
//
// Ownership analysis consumes LoweredPackage rather than raw storage:
// instruction spans locate diagnostics and the address table names
// places (analysis needs only identity, messages need names).
struct LoweredPackage {
  ir::Storage storage;
  // Parallel to storage instrs by InstructionIdx.
  std::vector<diag::Span> instr_spans;
  // Alloca additions: address, bound name, and whether it backs a
  // parameter (escape analysis treats parameters as external roots).
  struct AddrInfo {
    ir::RegisterIdx addr;
    std::string_view name;
    bool is_param = false;
  };
  std::vector<AddrInfo> addr_names;
};

diag::Fallible<LoweredPackage> lower_package(analyzer::CheckedPackage package,
                                             ir::PointerWidth width,
                                             str::StringInterner& strings,
                                             diag::DiagBag& bag);

}  // namespace lower
