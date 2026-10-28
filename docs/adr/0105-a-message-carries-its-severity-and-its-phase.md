# ADR-0105: A message carries its severity and its phase

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-28

## Context

Every diagnostic call site named three things about one message: its
`i18n::Key`, a `diag::Severity`, and a `diag::Stage`. The third was a
per-module constant in practice (Analyzer 271 sites, Pipeline 101, Pkg
39, Lowering 19, Borrow 9, Parser 5), and the second and third are
facts about the message rather than about the site: measured over the
444 sites whose arguments are literals, the severity is a function of
the key with **no** exception and the phase with three (the keys the
parser and the analyzer spell alike).

ADR-0039 and `src/i18n/README.md` said "the diagnostic code stays at
the call site", which was read as covering the phase and the severity
too. The catalog is the one list the key enum, the language tables,
and the completeness check are generated from, so a fact the message
owns has one home there.

## Decision

**The catalog entry carries the message's severity and phase. A call
site names the message, the code tooling matches on, and the
arguments.**

- `messages.def` entries are `F(Name, Severity, Stage, Text)`.
  `i18n::detail::KEY_META` is generated from the same list, so the two
  cannot disagree, and `DiagBag::emit<K>(code, [span,] args...)` reads
  it for the severity and the phase.
- **The code stays at the call site.** It is the identity a machine
  matches on, several codes carry more than one wording, and the
  linter's codes are a family of their own (`LintCode`), so the code
  is the site's to name.
- **A lint's severity stays at the site too.** A rule's level is its
  configuration's (`allow`, `warn`, `deny`), not the catalog's, so
  `emit_named` takes the severity and reads only the phase.
- Three keys that two phases spelled alike are split, because a
  message belongs to the phase that prints it:
  `Parser`/`AnalyzerInvalidSyntaxTree`,
  `Parser`/`AnalyzerNestingTooDeep`, and
  `Analyzer`/`ParserOrPatternBindsDifferentNames`; the lowering's own
  nesting message is `LowerNestingTooDeep`.
- A message only the cli renders carries `Note` and the pipeline
  phase, which nothing emits and so nothing reads.

## Consequences

- A call site is shorter and says only what the site decides: 459
  sites lost two arguments each, and the phase can no longer drift
  from the message that reports it.
- Changing which phase reports a message is a catalog edit, not a
  sweep over its call sites.
- `emit<K>` requires a constant key: a key a helper takes as a
  template parameter is fine (`emit_unresolved<Id>`, the lexer's
  `emit_error<K>`, the linter's `report<K>`), and each instantiation
  reads its own entry.

## Alternatives considered

- **Keep the triple at every site.** Rejected: two of the three were
  the catalog's own facts, repeated 459 times, and a message that
  moved between phases needed every site edited.
- **Derive the code from the key too.** Not taken: `LintCode` is a
  second code space, the linter's severity is configuration, and
  "several codes carry one wording" is why the catalog is keyed by
  wording rather than by code. The code is what a tool greps for and
  belongs where the check is written.
- **Derive only the phase, leaving severity at the site.** Rejected:
  the measurement says the severity has no per-site variation, so
  leaving it would keep 459 redundant arguments for no gain.
