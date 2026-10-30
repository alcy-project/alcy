// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: arithmetic as a capability.

// A nominal type implements the operators its arithmetic supports; a
// scalar uses the backend's own operators and needs no implementation.
// The operands pass by value, so `a + b` consumes both, which for a
// `Copy` type reads as a copy (ADR-0053).
pub spec Add {
  fn add(self: Self, other: Self) -> Self;
}

// `a - b`.
pub spec Sub {
  fn sub(self: Self, other: Self) -> Self;
}

// `a * b`.
pub spec Mul {
  fn mul(self: Self, other: Self) -> Self;
}

// `a / b`.
pub spec Div {
  fn div(self: Self, other: Self) -> Self;
}

// `a % b`.
pub spec Rem {
  fn rem(self: Self, other: Self) -> Self;
}

// `a ** b`. The integer scalar form is repeated squaring in the
// compiler; a nominal type defines its own meaning here.
pub spec Pow {
  fn pow(self: Self, other: Self) -> Self;
}

// `-a`.
pub spec Neg {
  fn neg(self: Self) -> Self;
}
