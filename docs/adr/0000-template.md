# ADR template

Copy this file to `NNNN-kebab-case-title.md` (next free number). Keep it
short: the decision and its consequences matter, not the process. `Subject`
says which of the two the record is about, which is what keeps this one log
readable once the implementation and the specification are told apart.

```markdown
# ADR-NNNN: Title

- Subject: the language | the compiler
- Status: Proposed | Accepted | Superseded by ADR-MMMM
- Date: YYYY-MM-DD

## Context

What problem forces a decision? What constraints apply
(`no_std` boundary, output-contract stability, crash safety, ...)?

## Decision

What we do, concretely (APIs, flags, schemas, defaults).

## Consequences

What this buys, what it costs, and what is explicitly out of scope.
Link follow-up TODOs instead of expanding scope here.
```
