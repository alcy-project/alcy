# ADR-0062: Arithmetic is defined

- Subject: the language
- Status: Accepted
- Date: 2026-10-12

## Context

`docs/spec/types.md` left division by zero and overshifts "unchecked
with backend-defined behavior", and the two backends disagreed: LLVM
treats `sdiv` by zero and a shift past the width as undefined, so
`-O3` may fold code that reaches one, while the wasm backend traps
on a zero divisor and masks a shift count in hardware. A program
could change meaning with the backend, and the optimizer was allowed
to assume away the branch a bounds check relies on. Compile-time
evaluation had already picked its own answers: a constant zero
divisor is an error, a constant signed `MIN / -1` wraps, and a
constant shift past 63 was refused. The front end and the backends
need one story, and the language's rule is that nothing is
undefined.

## Decision

**Every arithmetic operation has one value, on every backend.**

- A shift count rides modulo the type's width: `x << n` means
  `x << (n mod bits(x))`, and the same for `>>`. Lowering masks the
  count once, so no backend sees an overshift.
- Division or remainder by zero panics, with the message "division
  by zero". Lowering checks the divisor before the operation on
  every path, so a backend's undefined division by zero is
  unreachable; the panic is the runtime's defined abort.
- A signed `MIN / -1` wraps to `MIN`, and `MIN % -1` is zero,
  matching what compile-time evaluation already did. Lowering
  selects the wrapped result instead of letting the division
  overflow.
- Compile-time evaluation follows the same rules. A constant zero
  divisor stays a compile error, since a program that always panics
  at its first execution is a mistake worth naming while it is
  being written.

## Consequences

A shift with a non-constant count gains one `and`, and a division
gains a compare and a branch; a constant operand folds both away, so
hot code with literal counts is unchanged. Both backends now agree
with each other and with the comp engine, and the optimizer may not
assume either operation is undefined. The alternative — declaring
both undefined — was rejected: it lets an optimizer delete code a
program needs and leaves the two backends refusing different
programs.

Floating-point division by zero stays IEEE: infinity or NaN, which
is already a defined value.
