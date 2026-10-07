// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: equality as a capability.

// `==` reaches `eq` and `!=` its negation, so an implementation
// defines both. There is no type parameter: an equality is between two
// values of one type. Floats implement this and not `Eq`, because IEEE
// equality is not reflexive.
pub spec PartialEq {
  fn eq(self: &Self, other: &Self) -> bool;
}

// Reflexivity: `a == a` always holds. The spec declares no method of
// its own; an implementation is the promise, and it requires a
// `PartialEq` implementation for the same type (ADR-0053).
pub spec Eq: PartialEq {
}
