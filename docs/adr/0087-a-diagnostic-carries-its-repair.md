# ADR-0087: A diagnostic carries the repair its producer knows

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-28

## Context

A quick fix is an edit a client applies without asking: for a finding
whose repair is mechanical, the compiler knows the exact bytes to
change. Deriving that edit in the language server would mean the server
reading the finding's span and deciding, on its own, what the rule
meant -- a second reading of what the linter already knew, and one that
could disagree with it. ADR-0084 and ADR-0085 settle the same question
for names: the tool that decided the fact is the tool that reports it.

## Decision

- A diagnostic carries an optional machine-applicable repair:
  diag::Fix names the span to replace and the replacement bytes, empty
  to delete. The repair is a producer's assertion, so a diagnostic
  carries one only when its rule is sure of it.
- The bag owns the repair's bytes and hands out the record; a rule
  attaches one through its lint context beside the label it may already
  attach. A repair the arena cannot hold is dropped, leaving the
  finding whole, exactly as a label is.
- The language server maps the repairs to CodeActions. It reads the
  repair the finding carries; it does not decide what the repair is.
- A rule with no sure repair carries none. needless-return is the
  first: it deletes the keyword and the space before the value, leaving
  the value as the body's tail.

## Consequences

An action's edit is the linter's own repair, so applying it reproduces
what the rule meant and cannot drift from the finding that offered it.
Merging a bag copies the repair, so a run that collects per-thread bags
keeps its offers.

Coverage is what a rule is sure of. A finding whose repair needs a
rewrite the rule cannot state exactly stays advisory; adding one is a
change to that rule, at the place it reports, not a new pass.
