# ADR-0096: The concurrency promise ends at the thread

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

The suite reserves the concurrency packages by name and purpose:
`thread` (spawning and joining), `sync` (locks, once, channels),
`atomic` (operations and fences), `time` (clocks, deadlines,
durations), `network` (sockets and addresses), `arch` (target-specific
operations). Their contents arrive later, and the first one that hands
a value to another thread forces the question: what does the language
promise about values crossing a thread boundary, and where does that
promise end?

Rust answers with `Send` and `Sync` — sealed marker traits whose
implementations are, in the last resort, user-written and `unsafe`:
`unsafe impl Send for MyRawPointer` is a hand assertion the compiler
cannot check and reviewers cannot see. And the answer stops at the
process: a memory-mapped file or a shared segment is outside the
type system entirely.

## Decision

**The language's guarantee covers values shared between threads of one
process. Memory shared across a process boundary is outside it and is
the gate's business.**

- The across-thread markers (`Send`, `Sync`; names reserved) arrive
  with the first package that needs one, using ADR-0085's mechanism: a
  sealed marker derived structurally from the type's fields, with no
  user implementation and no negative implementation. A type opts out
  by holding a part that is thread-bound, never by an assertion.
- The blocking model is the base (`thread`, `sync`, `atomic`), and a
  value's thread-safety is a property of its type, decided once.
- A process boundary — a mapped file, a shared segment, a foreign
  thread entering through an FFI call — is not covered by the types.
  The package that offers such a view (`network`, a futex over a
  shared segment, a mapped file) states the contract in its own
  documentation, and using the view is an `unsafe` operation
  (ADR-0050, ADR-0100).
- The promise is also why unwinding is absent (ADR-0101): a thread
  cannot be observed mid-unwind, so a lock cannot be poisoned by an
  aborting owner.

## Consequences

- A value that crosses a thread boundary is checked at the boundary,
  and the check is structural rather than an assertion: a type that
  holds a thread-bound part is refused without anyone writing
  `unsafe impl`.
- Cross-process sharing is honest about needing a contract: the
  package documents it and the operation is gated, rather than the
  type system pretending to see it.
- The packages can land one at a time; nothing needs a marker on the
  first day, and the first that does brings the derivation with it.

## Alternatives considered

- **Hand-written `unsafe impl Send`/`Sync`, as Rust has.** Rejected:
  the assertion is unchecked, it is invisible at the use site, and
  the common cases (raw pointers in a wrapper) are exactly where it
  is wrong.
- **No markers; everything is shareable and the programmer sorts it
  out.** Rejected: it makes every data race a review question rather
  than a type error.
- **A process-wide memory model in the language.** Rejected: foreign
  mappings have no types, so the language would state a contract it
  cannot check; the packages own their contracts instead.
