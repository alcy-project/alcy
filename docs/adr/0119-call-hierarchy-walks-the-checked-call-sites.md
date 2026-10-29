# ADR-0119: Call hierarchy walks the checked call sites

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

A call hierarchy needs the calls of a package: which call site reaches
which declaration, and which function each site sits in. The checker
records a reference for every name it resolves, and a call site is one
of those references whose site is the callee of a call expression.

## Decision

- The language server builds the graph from the reference table. A
  reference is a call when its site is the callee of a CallExpr or the
  name of a MethodCallExpr, so a function named but not called is not a
  call edge.
- A call site's caller is the smallest function item whose span holds
  it; a callee is the declaration the reference resolved to.
- prepareCallHierarchy answers the callable at the cursor, and the two
  call requests round-trip its name and whole-item spans through the
  item's data field, so no state is kept between requests.

## Consequences

Incoming and outgoing calls come from the same records as definition,
references, and rename, including through a method call. The graph is
rebuilt per request from spans the checker owns; no second resolver and
no persistent index is introduced.
