# ADR-0089: Rename edits the segments the checker named

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

A rename must touch exactly the occurrences of one declaration and no
other text. The checker's reference table records a site span and the
declaration that site resolved to. A site may be a path whose segments
include names other than the one being renamed, so replacing a site's
whole span would drop the qualifier.

## Decision

- Rename reuses definition_at and references_at, so it edits the same
  occurrences the language server already reports.
- The declaration span is replaced directly: it is one identifier.
- A site is replaced by the identifier token inside it whose spelling
  equals the declaration's name. When the name occurs zero times in a
  site, or more than once, the rename is declined: the server answers
  null rather than guess which segment the checker meant.
- The new name is validated by lexing it, not by a second keyword list:
  it must be one identifier token covering the whole string. A keyword
  or a multi-token string is rejected with InvalidParams.

## Consequences

Renaming a recorded name is complete and conservative. A name the
reference table does not record, such as a field reached through a dot,
has no rename target, so the client never offers an edit that would
miss occurrences. An ambiguous site is refused rather than edited
wrongly, which keeps a rename from changing a name it did not resolve.
