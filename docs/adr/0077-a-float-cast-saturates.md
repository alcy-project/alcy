# ADR-0077: A float-to-integer cast saturates

- Subject: the language
- Status: Accepted
- Date: 2026-10-17

## Context

ADR-0066 settled the arithmetic the two backends answered differently,
but it left the cast from a float to an integer out, and the cast is the
same problem. LLVM's `fptosi` and `fptoui` are undefined for NaN and for
a value the destination cannot hold, so one program answered differently
in a checked build, a release build, and a shape that fed the poisoned
word into the checked formatter (`nan as i32` exited 1, then 120, then
aborted inside unrelated code); the direct-wasm backend wrote the
trapping `i32.trunc_f64_s`, so the same cast became a `RuntimeError` no
alcy diagnostic names. A program could change meaning with the backend,
and an optimizer was allowed to assume the cast never happened.

## Decision

**A cast between a float and an integer answers one value on every
backend: the value saturates.** A result below the destination's range
takes its minimum, above its range its maximum, and NaN becomes zero -
the semantics of `i32.trunc_sat_f64_s` and of LLVM's `llvm.fptosi.sat`
alike.

- The LLVM backend emits `llvm.fptosi.sat`/`llvm.fptoui.sat` where it
  emitted `fptosi`/`fptoui`.
- The direct-wasm backend emits the `0xFC`-prefixed saturating
  conversions where it emitted the trapping ones, and clamps the 32-bit
  result to the bounds of a narrower destination, which is where the
  LLVM intrinsic saturates directly.
- A value in range truncates toward zero, which both forms already did.

## Consequences

Every profile and both backends answer the same value for a cast, and no
cast is undefined, so an optimizer may not assume one away. The cost is
that an out-of-range cast is silent: a program that means to refuse one
compares before the cast. Wrapping and a panic were the alternatives;
neither backend has a wrapping conversion, and a panic would put a branch
on every cast. Saturating is what Rust's `as` does, so the spelling
matches the expectation a reader brings to it.
