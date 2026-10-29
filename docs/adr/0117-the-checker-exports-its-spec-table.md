# ADR-0117: The checker exports its spec table

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

An implementation request needs to answer which methods implement a
spec. The checker already holds both facts: the spec declarations, in
the order a MethodInfo's spec field indexes, and the impl blocks. Both
were private to the checker, so the language server could not read them.

## Decision

- CheckedPackage gains two small tables: SpecDecl (a spec's name and
  item) and SpecImplDecl (the spec it implements and the impl item),
  copied from the checker in the same order, so a MethodInfo's spec
  field still indexes the exported list.
- The language server reads them. A spec method or a call that resolved
  to one names the spec and the method, and every MethodInfo carrying
  the same spec and name is an implementation.
- A spec name answers every implementation of that spec.

## Consequences

Implementation joins definition, references, highlights, rename, and
semantic tokens on the checker's own reading. The exported tables are
data the checker already built, so no second pass and no name heuristic
is introduced.
