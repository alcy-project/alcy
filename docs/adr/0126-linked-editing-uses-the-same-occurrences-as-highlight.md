# ADR-0126: Linked editing uses the same occurrences as highlight

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

An editor can mirror an edit across every occurrence of a name while
the user types, which is a rename performed one keystroke at a time. It
needs the ranges that must change together, and a pattern that says
what an occurrence looks like.

## Decision

- The occurrences are the ones document highlight already answers: the
  declaration the cursor resolves to and every reference the checker
  recorded.
- They are returned as a LinkedEditingRanges object with the ranges and
  the identifier pattern, or null when the cursor resolves to nothing.
- A name the checker did not record gets no ranges, so the editor does
  not mirror a name it cannot follow.

## Consequences

Linked editing and rename cannot disagree about which occurrences
belong to a name, because they read one seam. The pattern confines the
mirroring to identifier-shaped edits.
