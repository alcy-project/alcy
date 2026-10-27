# ADR-0099: What 1.0 means

- Subject: the language
- Status: Accepted
- Date: 2026-10-27

## Context

`docs/spec/` marks sections MVP, Bootstrap, staged, and deferred, and
`docs/roadmap.md` tracks the work those markers name. None of that
says what the *number* 1.0 is about: whether it is a feature count, a
date, or a compatibility line. Without an answer, "we need this
before 1.0" is an argument that cannot be settled, and the promises
this series of records makes cannot be pointed at anything.

The window the series works in is the reason for the question: before
1.0 a decision is changed by editing the spec and the code, and a
wrong variant or name costs one commit. After 1.0 the same change
costs every dependent. The number is where that cost changes.

## Decision

**1.0 is the compatibility line: the point after which the promises
in this record series stop being editable. It is declared when the
conditions below hold, and not on a date.**

- The spec's MVP sections are implemented, and every staged or
  deferred marker is either implemented or moved to an explicit
  post-1.0 list in `docs/spec/deferred.md`. A marker that ships
  as-is is named as the experiment it is.
- Every Accepted ADR is implemented, or is explicitly deferred with
  the promise kept, and no Accepted ADR contradicts the code. The
  series this record closes — sizedness (0083), layering (0084),
  movability (0085), an end's order (0086), the niches (0087),
  naming (0088), aliasing (0089), orphan rules (0090), `str` (0091),
  conversions and dispatch (0092), `?` (0093), growth (0094) — is
  the frozen surface.
- The std suite's public surface is described in `docs/spec/`, not
  only in ADRs: the ADRs record why, and the spec records what.
- A program written against the suite needs no `unsafe` outside the
  standard library's own internals (ADR-0100): the escape hatches
  exist for the platform and for the library, not for the user's
  ordinary work.

Until 1.0, a wrong decision is fixed rather than kept: changing a
variant set (ADR-0094), a name (ADR-0088), or a layout costs a commit,
and the alternative — carrying a mistake for compatibility — is the
thing this series exists to avoid.

## Consequences

- "Before 1.0" is a technical statement, not a schedule: an argument
  to do something now is an argument that doing it later is a breaking
  change.
- The number is also the honesty line for staged markers: a feature
  that ships under a staged marker is not promised, and the promise
  starts when the spec says it does.
- After 1.0, breaking changes are a major version, and the growth
  rules (ADR-0094) are what makes most of them avoidable.

## Alternatives considered

- **1.0 = the MVP spec is implemented.** Rejected: MVP is a milestone
  inside the work; a milestone is not a compatibility line, and
  shipping it as 1.0 would freeze sections that were never designed
  to be promises.
- **1.0 = feature-complete.** Rejected: undefined, and it makes every
  request a release blocker.
- **1.0 on a date.** Rejected: it makes the compatibility line a
  function of the calendar rather than of the record.
