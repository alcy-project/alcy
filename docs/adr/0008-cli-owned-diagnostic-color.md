# ADR-0008: CLI-owned diagnostic color presentation

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

Diagnostics continue to be written through the existing stdout logger. The
renderer uses the ANSI 16-color palette for error, warning, note, path, line
number, and caret elements, with resets around each styled span.

## Consequences

- Pipeline stages produce diagnostics but do not choose colors or write them.
- CLI formatting remains the single owner of diagnostic output policy.
- `--color=auto`, `--color=always`, and `--color=never` use the existing
  terminal capability behavior.
- Rendering stays compatible with the existing formatter and does not add ANSI
  bytes to diagnostic data.
- Moving diagnostics to stderr, adding true-color themes, and interpreting ANSI
  markup in user text remain separate follow-up decisions.
