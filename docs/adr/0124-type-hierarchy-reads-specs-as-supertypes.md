# ADR-0124: Type hierarchy reads specs as supertypes

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

Alcy has no class inheritance; a type takes a capability by implementing
a spec. A caller that asks for a type's supertypes and subtypes means
the specs a type implements and the types that implement a spec.

## Decision

- The two tables the checker already exports answer the request:
  package.specs names a spec and package.spec_impls names an impl block
  and the spec it implements.
- An impl block's target is the path its impl spells, matched by the
  target's last segment, so a plain type name reaches its specs and a
  spec reaches the types whose impls name it.
- A spec's own supertype is the spec its super path names, when it
  refines one.
- A struct, enum, or spec is reported with the kind the protocol gives
  it, and its spans round-trip through the item data.

## Consequences

Navigation over capabilities uses the same records as implementation
and rename, so a type and its specs cannot disagree. A type the server
does not resolve, and a target that is not a simple path, get no entry.
