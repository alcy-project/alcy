# ADR-0008: CLI-owned diagnostic color presentation

- Subject: the compiler
- Status: Accepted
- Date: 2026-09-25

## Context

The diagnostic renderer already has a color option, but pipeline code emitted
all diagnostics without forwarding the CLI color policy. Terminal capability
detection and output emission also need clear ownership without making the
renderer depend on a terminal or output stream.

## Decision

The `cli` module resolves the invocation's `--color` mode into a terminal color
style and passes an explicit `diag::RenderOptions` value to the diagnostic
emitter. Diagnostic emission moves out of `pipeline` and into a CLI-owned
reporter. The `diag` renderer emits base ANSI SGR sequences and never queries
the terminal. `fpag::term` remains the source of terminal capability detection,
including Windows virtual-terminal initialization.

Diagnostics are written by the CLI-owned output path. The
renderer uses the ANSI 16-color palette for error, warning, note, path, line
number, and caret elements, with resets around each styled span.

Superseded in part by `docs/adr/0020-command-result-envelope.md`, which replaced the stdout logger with a
result envelope and a pair of renderers, and by
`docs/adr/0033-errors-on-stderr-results-on-stdout.md`, which answered the
follow-up below by putting diagnostics on standard error.

## Consequences

- Pipeline stages produce diagnostics but do not choose colors or write them.
- CLI formatting remains the single owner of diagnostic output policy.
- `--color=auto`, `--color=always`, and `--color=never` use the existing
  terminal capability behavior.
- Rendering stays compatible with the existing formatter and does not add ANSI
  bytes to diagnostic data.
- Moving diagnostics to stderr, adding true-color themes, and interpreting ANSI
  markup in user text remain separate follow-up decisions. The first is now
  `docs/adr/0033-errors-on-stderr-results-on-stdout.md`; the other two are
  still open.
