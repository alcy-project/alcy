// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/storage.h"

namespace overlay {

// What a checked operation aborts on.
enum class Check : u8 {
  // The instruction is not checked.
  None,
  // `+`, `-`, `*`: the result does not fit the type.
  Overflow,
  // A shift count at or past the type's width.
  ShiftRange,
  // A signed `MIN / -1`.
  MinOverOne,
};

// One instruction's check for this build. The definitions of ADR-0066
// stay in the IR; an entry only names the diagnostic a checked build
// adds.
struct Entry {
  Check kind = Check::None;
  // The operand a condition reads when the instruction's own operands
  // are not the values the source wrote: a masked shift's raw count, or
  // a signed division's raw divisor. Invalid when they answer.
  ir::OperandIdx operand = ir::OperandIdx::invalid();
  // The rendered `file:line:column` the abort names, interned in the
  // build's string table. Empty when no site was rendered.
  str::StringPoolId site = str::EMPTY_STRING_ID;
};

// The build's checks, parallel to the storage's instructions. The IR is
// borrowed, never copied or rewritten; a backend reads an entry in
// place of the profile (ADR-0068).
struct Overlay {
  std::vector<Entry> entries;
};

// Computes the overlay one build wants. With the checks off every entry
// stays `Check::None` and `sites` is not read; with them on, `sites`
// supplies the rendered spelling per instruction, parallel to the
// storage (an instruction past the span keeps the empty site).
Overlay build(const ir::Storage& storage,
              bool overflow_checks,
              std::span<const str::StringPoolId> sites);

}  // namespace overlay
