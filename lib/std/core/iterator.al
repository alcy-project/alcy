// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: iteration as a capability.

// A source of values, consumed one at a time. The receiver is the
// cursor: `next` yields `Some` until the source is exhausted, then
// `None`. The interval types are data, so a range iterates through a
// cursor of its own rather than by itself.
pub spec Iterator<T> {
  fn next(mut self: &mut Self) -> Option<T>;
}
