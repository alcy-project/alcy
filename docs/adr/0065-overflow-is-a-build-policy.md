# ADR-0065: Overflow is a build policy, not an IR property

- Subject: the language
- Status: Accepted
- Date: 2026-10-11

## Context

`docs/spec/types.md` said the source `+`, `-`, and `*` wrap in every
mode, division by zero and overshifts stay backend-defined, and no
overflow-check switch exists. A debug build that silently wraps hides
the bug a developer is looking at; a release build that pays for a
check pays for what it asked not to have.

The two behaviors have to coexist with one lowered program per
package. The IR is the unit a build cache stores, so content that
depends on the profile would key a second entry and force a second
pass over checking and borrow verification - exactly the work the
cache exists to skip. Compile-time evaluation is the same problem in
miniature: its results become IR content, so its arithmetic cannot
follow the profile either.

## Decision

The overflow policy is a property of the build, never of the IR:

1. **One IR per package.** Lowering emits the same instructions for
   every profile; only code generation reads the policy
   (`EmitRequest::overflow_checks`, set from `--release`).
2. **The source arithmetic means the language's operations.** A debug
   build lowers `+ - * / % << >>` with checks that abort through the
   panic path; a release build lowers them without. Release keeps
   today's semantics: `+ - *` wrap, and a zero divisor, `MIN / -1`,
   and an overshift stay backend-defined.
3. **The wrapping forms are explicit.** `IntAddWrap`, `IntSubWrap`,
   and `IntMulWrap` wrap in every profile, and core declares
   `wrapping_add`, `wrapping_sub`, and `wrapping_mul` over them. A
   compiler-generated or library step that relies on the wrap says so
   through these rather than through a source operator.
4. **Compile-time evaluation always runs checked.** An overflow in a
   constant expression is a diagnostic (`ED009`), and the wrapping
   intrinsics evaluate to the wrapped value. The AST engine and the
   IR interpreter share one rule, so the engines cannot disagree.
5. **The cache key excludes the policy.** It carries the source, the
   compiler version, the target (a pointer width changes types), and
   options that change IR meaning; the profile and the optimization
   level are applied after a hit.

Amended when the checks became an overlay (ADR-0068): the policy no
longer rides as `EmitRequest::overflow_checks`. One walk over the
shared IR turns `overflow-checks` into an overlay beside it, holding
the checks the build wants, and each backend realizes a check from
that table and never reads a profile. The definitions of ADR-0066
apply to every build, so a release build is defined as well - `+ - *`
wrap, a shift count masks, a zero divisor panics, and `MIN / -1`
wraps - and the checks add only the aborts a debug build wants. The
direct wasm backend realizes the checks too now, so the gap the
consequences below record is closed.

## Consequences

- One verified IR serves debug and release; a cache hit skips
  parsing, checking, and borrow verification for both, and re-runs
  only codegen, the optimizer, and the link.
- A checked build carries the three check helpers and the panic path
  in its object; a release build carries neither. The check is a call
  and an overflow flag in debug; release code is what it always was.
- `types.md`, `items.md`, `comp.md`, and `ir-format.md` state the same
  rules by reference to this ADR.
- The direct wasm backend does not lower the checks yet, so a debug
  wasm build wraps like a release one until it does; the gap is a
  backlog entry.
- `doubled_capacity` and `slots_for` no longer spell the largest
  `usize` as `0 - 1`: that subtraction is checked now. They take it
  through `wrapping_sub`.

## Alternatives considered

- **Profile-specific opcodes.** Rejected: debug and release would
  lower different IR, so the cache would hold two entries per package
  and pay for checking twice.
- **Check every arithmetic opcode, compiler-generated included.**
  Rejected: range cursors and the FNV-1a hash rely on wrapping, and a
  debug build would abort inside the library rather than at the
  program's own operation.
- **An instruction flag instead of opcodes.** Rejected: the IR
  dispatches on opcode through an arity table, text and binary forms,
  an interpreter, and two backends; a flag touches all of them and
  widens every instruction, while only the explicit wrapping forms
  need the distinction.
- **Keep backend-defined behavior in debug.** Rejected: the profile a
  developer runs is the one that should surface the overflow.
