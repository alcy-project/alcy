# ADR-0120: Code lenses count the checker's references

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

An editor can show how often a declaration is used above it. The
number is the count of references the checker recorded, and asking the
reference query once per declaration would walk the table once per
declaration.

## Decision

- The language server walks the reference table once, groups the sites
  by the declaration they resolved to, and reads every count from that
  group.
- A lens is placed on each function, method, struct, enum, static, and
  const the document declares, with the count and the locations the
  client's show-references command takes.
- Only references in the root file are counted, as every other feature
  filters spans by file.

## Consequences

One pass answers every lens, so the request stays linear in the
references however many declarations the document has. A declaration
with no references still gets a lens, which makes unused code visible.
