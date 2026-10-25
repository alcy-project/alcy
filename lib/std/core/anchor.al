// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the anchor, a value that never moves.

// A type that holds one in any field, element, or payload is not
// Movable (ADR-0085): its values are built in place, read through
// borrows, and ended in place. The value is zero-sized; what it says is
// that the value it sits in has an address of its own.
pub struct Anchor {
  pub held: (),
}
