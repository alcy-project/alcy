// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"
#include "ir/operand.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace lowering {

// An operand holding an integer constant of the given type. The lowering,
// the drop pass, and the sequences they share all spell immediates the
// same way, so the one spelling lives here.
[[nodiscard]] ir::OperandIdx imm_from_u64(ir::StorageBuilder& builder,
                                          ir::TypeTag tag,
                                          ir::TypeIdx type,
                                          u64 value);

}  // namespace lowering
