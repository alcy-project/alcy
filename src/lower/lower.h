// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "analyzer/types.h"
#include "diag/bag.h"
#include "fpag/str/string_interner.h"
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
diag::Fallible<ir::Storage> lower_package(analyzer::CheckedPackage package,
                                          ir::PointerWidth width,
                                          str::StringInterner& strings,
                                          diag::DiagBag& bag);

}  // namespace lower
