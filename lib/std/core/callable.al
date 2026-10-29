// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: calling as a capability.

// The capability behind the `fn(...)` spellings (ADR-0116): a value
// whose type carries the signature `P -> R` is callable, with `P` the
// parameters as one tuple. Every closure nominal implements it by
// construction - the implementation is the code the lowering registers
// for the literal - and `fn(A, B) -> R` in a signature names this bound
// for an anonymous parameter. The spec is sealed to the suite
// (ADR-0053): the compiler owns the implementations, so no user type
// opts in by writing one.
pub spec Fn<P, R> {
  fn call(self: &Self, args: P) -> R;
}
