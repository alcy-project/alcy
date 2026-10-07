// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: indexing as a capability.

// The capability behind `a[i]`: `I` is the index type and `O` the
// element type, and an index that names no element panics, the way
// array indexing behaves. The spec is sealed to the suite (ADR-0053),
// so the standard containers are the implementations that exist.
pub spec Index<I, O> {
  fn index(self: &Self, i: I) -> &O;
}

// The capability behind `a[i] = v`: the receiver is exclusive and so
// is the result.
pub spec IndexMut<I, O> {
  fn index_mut(mut self: &mut Self, i: I) -> &mut O;
}
