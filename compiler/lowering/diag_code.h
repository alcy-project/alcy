// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace lowering {

// The checks this component reports, counted from 1. The ids are its own:
// a component does not know how many components there are or what they
// are called, because `diag` turns (component, id) into the code a user
// reads. `D` is this component's letter; `diag/stage.h` has the
// rest and `compiler/docs/diagnostics.md` says what each check means.
//
// The first id is spelled because the rule is that ids start at 1: a
// zero id is never a check that exists, which is the ambiguity an
// optional code would otherwise have to answer for. The rest follow, so
// the order here is the order the codes were handed out in, and 999 is
// the ceiling - a component near it wants its checks split, not a wider
// field.
enum class DiagCode : u8 {
  Unsupported = 1,
  Internal,
  Unreachable,
  DropUnplaced,
  DiscardedDestructor,
  TooDeep,
};

}  // namespace lowering
