# ADR-0123: A semantic token range answers only what it covers

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

A client that shows one viewport does not need the whole buffer's
tokens. The full request classifies every token; a range request should
answer only the tokens the range covers, so the cost follows the
viewport.

## Decision

- The token walk moves into one function that takes a byte range. The
  full request passes the whole buffer; the range request passes the
  offsets its positions name.
- A lexeme is classified only when it starts inside the range, and a
  multi-line token is cut at the range end.
- The provider advertises range support beside full support.

## Consequences

The range answer is the full answer filtered, from the same lexer and
the same checker records, so the two cannot disagree about a token's
kind. The delta encoding is relative to the previous emitted token, so
a filtered list is still a valid token array.
