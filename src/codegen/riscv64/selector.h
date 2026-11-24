// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

#include "codegen/backend.h"
#include "codegen/riscv64/minst.h"
#include "codegen/riscv64/rodata.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/str/string_pool_id.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "overlay/overlay.h"

namespace codegen::riscv64 {

struct SelectContext {
  const ir::Storage& storage;
  const ir::SymbolTable& strings;
  std::span<const diag::Span> spans;
  const ir::Function& function;
  diag::DiagBag& bag;
  ir::PointerWidth width;
  RoData& rodata;
  const overlay::Overlay* overlay = nullptr;
  std::span<const str::StringPoolId> instr_site_names;
  bool backtrace = false;
};

[[nodiscard]] base::Result<Body, codegen::EmitError> select_function(
    const SelectContext& context);

}  // namespace codegen::riscv64
