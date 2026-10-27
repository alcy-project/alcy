# ADR-0102: `comp` does not touch the world

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

`comp` marks parameters and blocks that must be evaluated during
compilation. Its first client is `fmt`, whose format string is parsed
at compile time and expands to copy sequences. `docs/spec/comp.md`
lists the restrictions: no I/O, no `panic`, no reading statics, no
unbounded loops, a fixed evaluation quota, checked arithmetic during
evaluation.

The restrictions read as current limits. The decision they implement —
that compile-time evaluation is a bounded, deterministic evaluator
inside the language, never a way to run a program at build time — is
what keeps a build reproducible and a dependency readable. Rust's
counterpart is `build.rs` plus procedural macros: arbitrary code the
toolchain runs before the language's rules apply.

## Decision

**`comp` is a bounded, deterministic evaluator. It cannot touch the
world, and it is not a build-script language.**

- Evaluation reads the source and the values it was given: no I/O, no
  clock, no environment, no static storage, no foreign call. A build's
  result follows from the sources, the manifest, the target, and the
  toolchain, and from nothing else.
- A `panic` during evaluation is a compile-time diagnostic with a
  source site rather than an abort: the operation is wrong rather than
  the program's runtime being wrong.
- Evaluation is bounded: a fixed quota per function call and loop back
  edge, and exhausting it is an error. Checked arithmetic stays on, so
  a value that would wrap at runtime is refused at compile time
  (ADR-0065).
- There is no hook that runs user code for its side effects: no build
  script, no procedural macro (ADR-0097), no compile-time `exec`.
- If `comp` grows — receiver methods, type positions such as array
  lengths (`docs/spec/comp.md`'s deferred list) — it grows inside
  these bounds. Lifting a bound reopens this record.

## Consequences

- A build is a function of its inputs, which is what makes the cache
  and the summaries (ADR-0072, ADR-0078) coherent: nothing about the
  machine can change what was compiled.
- A dependency is data the compiler reads, not a program it runs. A
  reader can audit what a package does without executing it.
- The cost is stated: a compile-time computation that genuinely needs
  the environment (embedding a build timestamp, reading a file) has no
  spelling, and the program computes it at run time instead.

## Alternatives considered

- **Build scripts, as Cargo has.** Rejected: a build becomes a program
  to trust, its result depends on the machine, and the dependency
  graph becomes a graph of code to execute.
- **Procedural macros as the compile-time extension point.** Rejected
  in ADR-0097 for the same reason; `comp` is the language's answer.
- **Unbounded evaluation with a timeout.** Rejected: a timeout makes
  the build's result depend on the machine's speed.
