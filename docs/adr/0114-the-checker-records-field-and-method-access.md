# ADR-0114: The checker records field and method access

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

The reference table records the names the checker resolves through a
path. A field read, a struct-literal field, a destructuring field, and
a method call resolve a name that is not part of a path, so they were
absent from the table and every feature built on it: definition,
references, highlights, rename, and semantic tokens all stopped at the
dot.

## Decision

- The four places that resolve a member push a reference into the
  module the site is in: the field read in check_field, the method call
  in check_method_call, the field initializer in check_struct_expr, and
  the field of a struct pattern.
- The site is the member name's span in the expression; the declaration
  is the field's name span or the method item's name span.
- The reference is additive. Lowering is unchanged, and the site is one
  identifier, so a rename replaces it directly.

## Consequences

Definition, references, highlights, rename, and semantic tokens reach
the members the checker resolved, on the same seam as the other names.
A member the checker does not resolve, such as a tuple index, still has
no record. The site keeps the file it is in, so a member whose
declaration is in another file is filtered the way the LSP filters
every other span.
