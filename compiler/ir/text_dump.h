// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>

#include "ir/write_input.h"

namespace ir {

// The lowered package as the text view `--emit=ir` writes. The form is
// defined by `compiler/docs/ir-format.md` (ADR-0056): a view for people,
// write-only, with storage indices preserved. The same input dumps to
// the same text.
[[nodiscard]] std::string write_text(const WriteInput& input);

}  // namespace ir
