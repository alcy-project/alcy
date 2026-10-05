# ADR-0033: Errors on standard error, results on standard output

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-02

## Context

`alcy run` writes the program's own output to standard output, and
`docs/adr/0008-cli-owned-diagnostic-color.md` already put the announcement
on standard error so that `alcy run | grep` would see the program and
nothing else. Everything else went to standard output: the diagnostics
from a failed compile, the warning from a successful one, the result line,
and the JSON document.

That made the two streams mean different things depending on the verb, and
it meant a diagnostic and a result shared one file descriptor, so a
reader who redirected one got both. It also meant the renderer had no
choice to make about where a message went, because there was nowhere else
for it to go.

`docs/adr/0008-cli-owned-diagnostic-color.md` left "moving diagnostics to
stderr" as a follow-up decision. This is that decision.

## Decision

A human-readable message the cli produces is either a result or an error,
and the stream says which:

- Diagnostics, warnings, notes, and the envelope's own failure go to
  standard error. `alcy check > report.txt` leaves a clean file; `alcy run |
  grep` never sees a compiler message at all.
- The result line, the time-trace summary, `--help`, and `--version` go to
  standard output. They are the answer to the question that was asked.
- `--json` puts the whole document on standard output and nothing on
  standard error, because a tool parsing it needs one document and the
  diagnostics are fields of it.

Each half is styled by the stream it lands on, so redirecting one of them
does not decide whether the other is readable.

Every human-readable error the cli itself produces - a rejected flag
combination, a parse error, an unknown verb - is a `diag::Diagnostic` with
no span and no code, rendered by `diag::render` like any other. The marker
is the renderer's and the sentence is the catalogue's; no path formats its
own `error: `.

Writes go through `cli::Logger`, which is a destination and one method. A
block is non-empty and ends in exactly one newline, a contract the producer
owns and `cli::is_block` states.

## Consequences

- There is one shape for an error line, so a reader who has seen one has
  seen all of them, and a tool that matches the `error[EC016]:` prefix
  matches compiler and cli errors alike.
- A codeless message renders `error:` and then the message, rather
  than a number nobody allocated. `--json` reports `"code":null` where a
  number would imply one.
- Writing a diagnostic no longer means choosing a stream at the call site.
  Adding a diagnostic to a command cannot break a consumer's redirect.
- `--json` is the only way to get diagnostics and the result together as
  data, which is the point of it.
- The result line is still on standard output, so `output=$(alcy build)`
  keeps working. Only the complaints moved.
- A consumer that scraped diagnostics from standard output has to read
  standard error instead. This is the one behavioural break, and it is the
  point of the decision.
- `alcy::Logger` does not know about severity, colour, or JSON. Formatting
  belongs to `diag::render`, and a writer that carried the styling would be
  a second place to change it.
