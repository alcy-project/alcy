# ADR-0103: One execution model

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

The suite reserves the concurrency packages: `thread` (spawning and
joining), `sync` (locks, once, channels), `atomic` (operations and
fences), and the async question behind them. Rust's regret is the
answer it gave: std has no runtime, so `Future`, `Context`, and
`Waker` are a vocabulary every runtime implements differently, a
library that wants to be runtime-agnostic compiles against two of
them, and a program's behavior depends on which executor it chose.

The language can answer differently because it has no ecosystem to
preserve: whatever std blesses is the model, and a second one is not a
supported shape.

## Decision

**There is one execution model. The blocking one is the base; if
`async`/`await` lands, exactly one executor lives in std and the
language provides the sugar.**

- `thread`, `sync`, and `atomic` are the model that exists: an
  operation blocks the thread it runs on, and a thread is the unit
  the language's guarantees are stated per (ADR-0096).
- Async is not in 1.0. When it lands, the executor is std's, the
  vocabulary (`Future`-shaped or otherwise) is std's, and `await` is
  a language form. There are no pluggable runtimes: a library names
  the one model, and a program does not choose an executor.
- A crate-shaped "runtime-agnostic" layer is not a supported goal. A
  library that must work under another scheduler is a port, not a
  configuration.
- The marker specs that say a value may cross a thread are structural
  (ADR-0096), so the async work inherits its thread-safety story
  rather than inventing one.

## Consequences

- The vocabulary has one home: a future-shaped type, a task, and a
  spawn are the standard library's names, and a library's signatures
  mean one thing.
- The cost is real and is accepted: an environment that wants a
  different scheduler (an embedded loop, a game engine's frame) does
  not plug one in; it either uses the standard model or writes its own
  concurrency with the primitives (`atomic`, `sync`) and no `await`.
- Deferring the design until the first real client keeps the shape
  from being guessed, and this record fixes the *count*, which is the
  part a later redesign cannot change cheaply.

## Alternatives considered

- **No runtime in std, runtime-agnostic traits, as Rust has.**
  Rejected: the vocabulary is fragmented, a library supports N
  runtimes or is unusable in some programs, and the choice is a
  configuration flag (which ADR-0098 refuses).
- **Green threads and a mandatory scheduler now.** Rejected: it
  imposes a runtime on every program including the ones that never
  await, and the blocking base is already shipped.
- **Both models supported permanently.** Rejected: two models is the
  split this record exists to avoid.
