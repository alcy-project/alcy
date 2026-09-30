# ADR-0019: Per-Goal Configuration Lives in `.alcy/`

- Subject: the compiler
- Status: Accepted
- Date: 2026-09-28

## Context

`alcy.toml` declares what a package is: identity, targets,
dependencies, module membership. Build choices kept growing next to
it: which link driver this machine uses, and later profiles, lint,
format, and LSP settings. A second question arrived with
dependencies: when package B builds package A as a dependency, whose
choices apply?

## Decision

- `alcy.toml` keeps package identity. Everything about building the
  package *as the goal* lives in `.alcy/` beside it, one file per
  concern: `toolchain.toml` now, `profile.toml`, `lint.toml`,
  `format.toml`, and `lsp.toml` later.
- Only the goal package's `.alcy/` is ever read. A dependency's
  `.alcy/` is invisible to the packages that build it.
- `toolchain.toml` holds two optional keys, `linker` and `link-args`.
  Absent file or absent key means the default driver and an empty
  argument list. A non-empty `--linker` overrides the file's driver; a
  `--link-args` given at least once replaces the file's list rather than
  joining it, so an invocation resolves to settings its own command and
  the file together can predict. `compile` keeps both flags and never
  reads files, so a single file stays reproducible from command and file
  alone. `link-args` are arguments for the driver — a library, a search
  path, a switch such as `-fuse-ld=lld` — and a flag meant for the
  linker behind it goes through `-Wl,`.
- Unknown keys are ignored, like `alcy.toml`. `new` and `init` do not
  scaffold `.alcy/`.

## Consequences

`check --stdin`'s removal already pushed editors toward files; this
pushes machine-local choices out of the manifest with the same
motive. The pipeline does not build dependencies yet, so the
visibility rule is recorded here rather than enforced.
