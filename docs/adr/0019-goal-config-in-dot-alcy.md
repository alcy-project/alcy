# ADR-0019: Per-Goal Configuration Lives in `.alcy/`

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
- `toolchain.toml` v1 holds one optional key, `linker`. Absent file
  or absent key means the default driver. A non-empty `--linker`
  overrides the file; `compile` keeps the flag and never reads files,
  so a single file stays reproducible from command and file alone.
- Unknown keys are ignored, like `alcy.toml`. `new` and `init` do not
  scaffold `.alcy/`.

## Consequences

`check --stdin`'s removal already pushed editors toward files; this
pushes machine-local choices out of the manifest with the same
motive. The pipeline does not build dependencies yet, so the
visibility rule is recorded here rather than enforced.
