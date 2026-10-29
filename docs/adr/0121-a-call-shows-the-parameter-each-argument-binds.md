# ADR-0121: A call shows the parameter each argument binds

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

An argument at a call site is a bare expression; naming the parameter
it binds is a reading the checker already has. The callee is a
reference the checker recorded, and the parameter names sit on the
callee's AST item.

## Decision

- The language server maps the reference table's call sites to the
  items their declarations name, then reads each item's parameter
  patterns.
- A hint is placed before an argument when the argument does not
  already spell the parameter name, so an expression that repeats the
  name is not decorated with it.
- A method call's arguments line up after the receiver's self
  parameter, which is named in the pattern like any other.

## Consequences

The hint is the checker's binding, not a guess from the argument's
shape. A call the checker did not resolve, and an argument beyond the
declared parameters, get no hint.
