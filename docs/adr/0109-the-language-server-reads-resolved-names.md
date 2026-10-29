# ADR-0109: The language server reads the checker's resolved names

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-28

## Context

Go to definition and find references need an answer to "which
declaration does this name refer to?" Only the checker knows: it walks
the scope stack for locals, reads imports and module prefixes for items,
matches the receiver for methods, and unpacks enum paths for variants.
None of that is a pure function of a name and a file, so the language
server cannot recompute it without becoming a second resolver beside the
checker -- the failure ADR-0081 and ADR-0107 refuse for parsers, for the
same reason: two implementations drift, and the editor's answer stops
matching the compiler's.

The checked package already tables the results of resolution for
lowering: which function a call reaches, which variant a path names,
which type an expression has. Those tables are keyed by the use, not by
a source offset, and they do not cover locals, statics, or type paths,
so they are not the query the editor needs.

## Decision

- The checker records, for each name it resolves, a pair of spans: the
  site the source wrote and the declaration the name resolved to. The
  record lives on the checked module beside the other side tables, so it
  is built in the one pass that already resolves the name and costs a
  push where the resolution happened.
- A declaration is recorded at the span a reader would point at: an
  item's name, a variant's name, a field's name, or a binding's pattern
  name. A local gains the span of the identifier its pattern wrote; a
  static gains its declaring item, so the reference names the item and
  not a copy of the name.
- Name resolution in lsp/ reads this table. Definition is the
  declaration of the reference whose site holds the cursor; references
  is every reference that resolved to the same declaration. The server
  does not walk scopes, imports, or impls to decide what a name means.
- The AST supplies the declaration side: when the cursor sits on a
  declaration rather than a use, lsp/ matches the position against the
  name spans the AST already holds, so a request on a declaration works
  without a use to start from.

## Consequences

A definition or reference is the checker's own answer, so it is exactly
what the compiler would bind, including through imports and generic
instantiations. The table grows with every checked instantiation, like
the other side tables; the LSP de-duplicates identical sites when it
answers.

Coverage is what the checker resolves: items (functions, types,
statics, consts), enum variants, methods, and bindings through a path,
plus field and method access recorded at the member site (ADR-0114).
The table does not replace the lowering tables: lowering still reads
the entries it was built against, and this record is additive.
