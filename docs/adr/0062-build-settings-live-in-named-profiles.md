# ADR-0062: Build settings live in named profiles

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-11

## Context

`--release` is the only switch for how a program is generated: on, the
middle end runs its O3 pipeline; off, it does not. Every other
code-generation behavior on the way — a panic that carries a backtrace,
a pinned backend (ADR-0049), debug information, frame pointers — would
otherwise land as its own flag, and a build worth reproducing would be a
pile of them. ADR-0019 reserved `.alcy/profile.toml` for exactly this,
"one file per concern", and ADR-0049 recorded that a persistent
`backend` setting waits for "the profile concept".

## Decision

A **profile** is a named set of build settings. Two are built in:
`debug`, the default, and `release`, which is what `--release` has
always meant. `.alcy/profile.toml` declares more profiles and overrides
the built-in ones, one table per name:

```toml
[debug]
optimize = true

[fast]
optimize = true
```

- Profile keys are code-generation semantics: settings under which the
  same source produces a different program. Today those are `optimize`
  and `on-panic` (which makes a panic walk the frame chain before it
  aborts; see ADR-0063). A future key (a pinned `backend`, debug
  information) arrives with the ADR that justifies it. Selection-only
  knobs (`--target`, `--emit`, `--deps`, `--backend` on the command
  line) and machine configuration (the link driver in `toolchain.toml`)
  are not profile keys: a profile has to mean the same thing on every
  machine.
- Unknown keys are ignored, like alcy.toml; a known key with the wrong
  type is an error, and a key at the top level that is not a table is
  not a profile.
- Resolution order is built-in values, then the selected name's table,
  then the command line's per-key flags. There is no inheritance: a
  profile the file declares starts from the same baseline as `debug`
  and never from `release`; a variant of release says
  `optimize = true` itself.
- `--profile <name>` selects, on `build`, `compile`, and `run`.
  `--release` stays as an alias for `--profile release`; giving both
  explicitly is refused rather than silently resolved.
- `compile` reads no `.alcy/profile.toml` (ADR-0019), so it selects a
  built-in and overrides keys, and a name only the file could declare
  is unknown there. `build` and `run` read the goal package's file and
  only the goal package's; a dependency's `.alcy/` stays invisible.

## Consequences

- A new code-generation setting is one key and one resolution step, not
  one flag per verb, and a build is reproducible by naming one profile
  (with the file that defines it).
- `--release` keeps its exact meaning: `--profile release` and
  `--release` produce the same object. The existing callers
  (`benchmarks/`, the docs, the tests) keep working unchanged.
- The name `debug` sits beside the compiler's own debug builds; the
  docs say "program profile" where the two could be confused.
- A profile cannot pin `--target`; cross-building stays an invocation
  choice. When `backend` becomes a key, a profile that pins a backend
  with no optimizer and also sets `optimize` is answered by that
  backend's own refusal, not by a silent downgrade — which is the
  behavior `--release` has with a named backend today.
- Profile selection has no persistent default yet: the file declares
  option spaces and the command line selects. A `default` key is
  possible later without changing the file format.

## Alternatives considered

- **One flag per setting.** Rejected: the pile grows with every
  feature, and a reproducible release build becomes a command line
  nobody records.
- **Inheritance (`fast = release + ...`).** Rejected for this version:
  resolution stays flat, and every profile reads completely in one
  table. It can be added later without changing what a table means.
- **A selection default in the file.** Deferred: with none, a build is
  reproducible from the command line and the file together, which is
  the property ADR-0019 asks of `compile`.
- **One file per setting under `.alcy/`.** Rejected: settings share
  one resolution order and one selection, and splitting them across
  files would spread a profile over several of them.

## Reference

- `compiler/pkg/profile.{h,cc}` parses the file; the built-in bases and
  resolution live in `compiler/pipeline/profile.{h,cc}`.
- `docs/adr/0019-goal-config-in-dot-alcy.md`: where goal
  configuration lives, and why `compile` reads no file.
- `docs/adr/0049-the-direct-backends-and-the-playground.md`: the
  persistent backend setting waits for this concept.
