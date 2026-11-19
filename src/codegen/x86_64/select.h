// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>

#include "codegen/backend.h"
#include "codegen/x86_64/minst.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"

namespace codegen::x86_64 {

// Selects one function's body: verified IR in, machine instructions out,
// with a virtual register for every IR register the body reads. The
// three-address shapes the IR already carries are kept, so allocation is
// the only stage that knows what a register costs.
//
// A construct the selector has no lowering for is reported at the place
// it was written, and the failure names it rather than guessing.
[[nodiscard]] base::Result<Body, codegen::EmitError> select_function(
    const ir::Storage& storage,
    const ir::SymbolTable& strings,
    std::span<const diag::Span> spans,
    const ir::Function& function,
    diag::DiagBag& bag,
    ir::PointerWidth width);

}  // namespace codegen::x86_64
