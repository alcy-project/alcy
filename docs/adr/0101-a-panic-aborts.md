# ADR-0101: A panic aborts

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

`docs/spec/errors.md` says a panic writes its message and aborts the
process, and that drops do not run on the panic path. ADR-0070 adds a
printing profile: an abort may list the frames it walked through. What
neither states is the decision that this is permanent — that there is
no unwinding to add later, and no recovery facility to build on top.

Rust's regret is `panic = "unwind"` as the default: every function has
two exit paths, `Drop` becomes observable mid-panic, `UnwindSafe` and
lock poisoning exist to describe a state nobody wanted to be in, and
`catch_unwind` is a recovery mechanism around broken invariants. The
abort default exists because unwinding's costs are real, and the
ecosystem's split shows the two answers cannot both be the language's.

## Decision

**A panic aborts. There is no unwinding, no destructor runs on the
panic path, and no facility catches a panic.**

- `panic(msg)` writes `panic at <file>:<line>:<column>: <msg>` and
  aborts; the entry thunk's fixed message and an allocation failure
  write the site-less form (ADR-0070).
- Values the process owned at the abort are not ended. The language
  already has no `forget` (ADR-0086), so the abort is the only leak,
  and it is a process that is ending anyway.
- No `catch_unwind`, `resume_unwind`, panic hooks, or panic payloads.
  A library that must recover from a failing operation returns a
  value — `Option`, `Result`, or its own enum (ADR-0093) — and the
  caller decides; that is the recovery story, and it is checked.
- The backtrace profile (ADR-0070) is the only knob around the
  mechanism, and it changes what is printed, not what happens.

## Consequences

- One exit path per function: code size and the shape of every
  generated function follow from the decision, and a destructor has
  one place to run.
- A lock has no poisoned state (ADR-0096): its owner cannot die while
  the process continues.
- Recovery is a value's job. An operation whose failure a caller
  handles says so in its type, and the failure's cause is preserved
  rather than flattened into a message.

## Alternatives considered

- **Unwinding with `catch_unwind`, as Rust has.** Rejected: two exit
  paths, destructors on a path the author did not write, and a
  recovery mechanism around states the type system calls valid.
- **Unwinding without catching.** Rejected: it pays every cost of
  unwinding and provides the recovery shape anyway, through
  destructors whose work nobody asked for.
- **A configurable abort/unwind switch, as Cargo's profiles have.**
  Rejected: a library cannot be written against two answers; the
  other ecosystem's split is the evidence.
