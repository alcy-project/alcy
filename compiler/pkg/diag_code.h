// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace pkg {

// The checks this component reports, counted from 1. The ids are its own:
// a component does not know how many components there are or what they
// are called, because `diag` turns (component, id) into the code a user
// reads. `H` is this component's letter; `diag/stage.h` has the
// rest and `compiler/docs/diagnostics.md` says what each check means.
//
// Every id is spelled out rather than left to count from the one above.
// A reader then sees a check's code without counting lines, and
// inserting a check in the middle shows up as a diff to every id after
// it instead of shifting them silently.
//
// Ids start at 1: a zero id is never a check that exists, which is the
// ambiguity an optional code would otherwise have to answer for. 255 is
// the ceiling, which is what the `u8` gives; a component near it wants
// its checks split, not a wider field.
enum class DiagCode : u8 {
  ManifestSyntaxError = 1,
  ManifestSemanticError = 2,
  ToolchainSyntaxError = 3,
  ToolchainSemanticError = 4,
};

}  // namespace pkg
