# ADR-0020: One command result envelope for every output path

- Subject: the compiler
- Status: Accepted
- Date: 2026-09-29

## Context

`alcy` reported its results by writing lines through `base::logger`, an
alias for fpag's `SyncLogger<StdoutSink, LogLevel>`. Six of the eight call
sites used `wo_prefix`, one used `error`, and nothing used any other
level: the level filtering, the sink abstraction, the per-record
timestamp, and every other capability of that machinery went unused.
Terminal detection was the one thing actually needed, and it already
lives in `fpag::term`.

The next output features do not fit a logger. Statistics belong in the
result rather than in a formatted sentence. Machine-readable output is
not another level: `--json` promises that standard output is exactly one
JSON document, which is a statement about the whole channel, not about
one message. A logger expresses that by testing a flag at every call
site, which puts the contract where the next contributor will forget
it.

`docs/adr/0008-cli-owned-diagnostic-color.md` recorded the same observation in reverse: it assigned color
policy to the cli while leaving emission to "the existing stdout
logger", and deferred moving diagnostics to standard error as a separate
decision.

## Decision

A command produces a single `cli::Envelope` describing what happened:
its status, its diagnostics, its statistics, an optional human message,
and, when tracing ran, the profile events. Envelopes are rendered by
one of two functions in the new `compiler/cli/output.{h,cc}`:

- `render_text` writes diagnostics through `diag::render`, then a
  result line, then the time-trace summary.
- `render_json` writes one JSON object to standard output and nothing
  else.

Every verb builds an envelope, including the ones with no consumer for
machine-readable output today, so that adding a result field means
filling in a field rather than finding the call sites that print.

`--json` applies to `build`, `compile`, and `check`. Help, version, and
parse failures keep their text form: a parse error is reported before
there is a command, and the rendered help is documentation rather than
a result.

`--time-trace` composes with it rather than replacing it. On its own it
prints a human-readable summary of the recorded phases; with `--json`
the events are embedded in the envelope and no file is written. The
`traceEvents` array sits at the top level of the document, which is what
Perfetto and the Chrome tracing viewer read, so the captured output can
be pasted into a trace viewer unchanged while `diagnostics` and `stats`
remain available to a tool reading the same document. The envelope
therefore avoids Perfetto's own reserved keys: `traceEvents`,
`displayTimeUnit`, `metadata`, `systemTraceEvents`, `stackFrames`,
`samples`, `cpuProfileData`, `heaps`, `cpuSamplingData`, and
`objectsUrls`.

Time is measured once, in `cli_main`, with `std::chrono::steady_clock`.
The profiler's own clock is wall-clock derived and is not used for it.

`base::logger` and `compiler/base/logger.{h,cc}` are removed, along with the
`fpag/logging` dependency. Two `fpag` facilities stay, because neither
is about output: `term::console_color_style` for terminal capability
detection, which `docs/adr/0008-cli-owned-diagnostic-color.md` assigned to `fpag::term` and which includes
Windows virtual-terminal initialization, and `debug::init_debug_logger`,
which is what makes a failed `FPAG_CHECK` inside fpag report itself.

## Consequences

- Adding a result field touches the envelope and its renderers, not
  every command that prints.
- The JSON document is the contract an editor integration reads; the
  text line is for a person at a terminal.
- A trace is no longer a second artifact to correlate, and the file
  naming beside the output is gone.
- `alcy_print` still writes in Windows text mode. Any output path that
  redirects a program's bytes is a separate concern.
