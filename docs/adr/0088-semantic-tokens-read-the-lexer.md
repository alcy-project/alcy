# ADR-0088: Semantic tokens read the lexer and the checker's names

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

Modern editors highlight with semantic tokens rather than inferring
everything from a text grammar, and the point of the compiler doing it
is that a name's color reflects what the compiler actually resolved it
to, not how it happens to be spelled. Two facts are needed: the exact
tokens, including the trivia a grammar-based highlighter keeps, and the
kind of each name.

## Decision

- The tokens come from cst::lex, the lossless stream the formatter and
  folding already read. It reports every token and every comment with
  its span, so highlighting does not depend on a parse succeeding.
- Token kinds that are lexical -- keywords, primitive type names,
  literals, comments, and operators -- come from the lexer's TokenKind
  directly; no analysis is needed and a broken buffer still highlights.
- An identifier's kind is the checker's answer where it recorded one:
  the declaration spans of items, fields, and variants, and the
  reference table's declaration and site spans, with a path's last
  segment taking the kind of the name the path resolved to. A name the
  reference table does not cover falls back to the module's name
  tables.
- The legend order is the wire format: a token's type on the wire is its
  index in the server's advertised tokenTypes array, so the two are
  defined together and cannot drift.
- A token never crosses a line on the wire, so a block comment or a
  multi-line string is emitted one line at a time.

## Consequences

Highlighting is the compiler's reading of the buffer: a local is a
variable because the checker bound it there, and a name that resolves
to a struct is colored as one. The lexer-driven part is available even
when analysis fails, so a buffer being edited still highlights.
Positions and lengths are UTF-16 code units, converted through the
document's position index like every other feature.

Coverage is what the checker records. A name reached through a dot is
colored as the field or method the checker resolved, because the member
site carries a reference like any other name (ADR-0090).
