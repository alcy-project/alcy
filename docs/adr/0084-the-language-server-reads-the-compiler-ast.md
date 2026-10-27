# ADR-0084: The language server reads the compiler's AST

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-28

## Context

The language server's outline, hover, and the features after them need
the grammatical structure a reader sees: which items a file declares,
their names and kinds, where a struct's fields are, which functions an
impl holds. The toolchain has two trees. ast/ is the compiler's typed
tree: an item carries a kind, a name, a span, and the doc comments
written above it, and the analyzer attaches types to it. cst/ is the
formatter's lossless tree: tokens, comments, and newlines under nested
delimiters, with no item kinds and no names, because layout does not
need them.

## Decision

- A syntax feature reads the compiler's parse (pipeline::parse_files)
  and walks ast::ItemNode, the same tree the analyzer checks. The
  outline is the first such feature, and the features after it reuse
  the same parse.
- A trivia feature reads cst/: folding ranges, semantic tokens for
  comments, and selection ranges are about the bytes and the whitespace
  between them, which only the lossless tree keeps.
- Neither tree grows the other's information. cst/ does not learn
  grammar or names, which would be a second parser beside the first,
  the failure mode ADR-0081 and ADR-0083 both refuse. ast/ does not
  learn trivia, which would put comment bytes and whitespace on the
  compiler's hot path for the editor's benefit.
- A semantic feature reads the checked package, not a second reading of
  the source. Where the analyzer has no answer keyed by a source offset,
  the seam is added to the analyzer (a later record), not re-derived in
  lsp/.
- The parse and the checked package are cached per document version as
  the features that need them land, so a request does not repeat what
  the previous one already built.

## Consequences

The outline is correct by construction: it reports the items the
compiler accepted, with the spans and doc comments the compiler holds,
and it survives the semantic errors that stop analysis because it stops
at the parse. The cost is one parse per requested version until the
cache lands. parse_files is the entry the compiler's own check uses, so
the server and the compiler cannot disagree about where an item is.

Out of scope for this record: the public offset-keyed resolver the
definition and references work needs; semantic tokens beyond comments
and keywords; and any tree a host other than the compiler builds.
