# ADR-0032: A Range End Is Spelled `..<` or `..=`

- Subject: the language
- Status: Accepted
- Date: 2026-10-02
- Supersedes in part: `docs/adr/0025-ranges-as-data.md` (its
  "`..` and `..<` are the same operator" rule)

## Context

ADR-0025 froze `..`, `..=`, and `..<` as three spellings of one
operator, with `..` meaning what `..<` means. That is a defensible
default, and it is what most languages do — but it is also the
default that hides the one decision a range end encodes. `0..n` reads
as inclusive to anyone coming from Python or from mathematical
notation, and the reader has to know the language's rule to notice
that it is not. Meanwhile the interval *data* has always carried the
distinction faithfully: `Bound` has an `Excluded` and an `Included`
variant, and the index decoder reads them.

The cost of asking for the distinction is noise. `&a[1..3]` becomes
`&a[1..<3]`, and the language trades a common, short spelling for a
self-evident one. The language has no users and no self-hosted source
yet, so this is the cheapest moment the change can ever be made; after
self-hosting, every `..` in the compiler's own source is a migration.

The start side needs no marker and never had one: there is no syntax
that excludes a start endpoint, and the desugaring writes
`Included(start)` unconditionally. Only the end side is ambiguous.

## Decision

**A range with an end endpoint spells it.** `..<` and `..=` are the
only operators that may be followed by an endpoint; bare `..` with an
endpoint is rejected where it is written, with a diagnostic naming
both spellings. `for i in 0..<n` and `for i in 0..=n` are the whole
surface for an inclusive/exclusive choice, and neither is guessable
from the other.

**Bare `..` remains the unbounded spelling.** `1..`, `..`, `a[..]`,
and `a[1..]` name a range with no end, and `..` is the only way to
write that. `..<` and `..=` are for the present endpoint, so the rule
is "an operator that names an end says how", not "one operator per
shape". A start endpoint keeps no marker of its own.

**The rule is lexical, not semantic.** The parser decides, at the
token: `..` followed by something that starts an endpoint is an error,
whatever the endpoint turns out to mean. Nothing in the checker or
lowering changes, because the AST already records the end's inclusion
as a single flag and the lexer already produces three distinct tokens.

## Consequences

What this buys is that the one asymmetry in a range is visible at the
call site, and that `Range`'s three `Bound` variants all have a
spelling a program can produce. The start side's permanence is now
stated in the specification rather than only implied by the absence of
syntax.

What it costs is one character on every run and range expression
with an end, and the loss of `a[1..3]` as the way most people will
first write a run. The migration is mechanical and bounded: it
touches the range cases, the `subslice` borrow-rule cases, the parser
and checker unit tests, and the documentation, all within one commit,
since no released program can contain the old spelling.

The rejection gets its own diagnostic code rather than the generic
unexpected-token one, so the registry names the rule and a future
reader can tell a misspelled end from a malformed endpoint. It is
reported at the operator rather than at the endpoint, so a file with
several bare `..` needs several replacements. `..` on its own — as in
`..` and `a[..]` — is untouched, as is the struct-update `..base`,
which is a different construct parsed in a different position.
