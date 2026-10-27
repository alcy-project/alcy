# lsp

The language server: `alcy lsp` speaks LSP over standard input and
output. The module is a library over the toolchain rather than a stage
of the compiler (ADR-0083), so it owns no analysis of its own:

- `frame.{h,cc}` is the base protocol: a `Content-Length` header and
  a JSON-RPC payload per frame.
- `transport.{h,cc}` reads and writes those frames over a descriptor
  pair and buffers the reads.
- `document.{h,cc}` holds the open buffers, applies incremental
  changes in protocol order, and maps LSP positions to byte offsets.
- `folding.{h,cc}` folds every multi-line brace pair and every run of
  comments on adjacent lines, from the lossless CST.
- `analysis.{h,cc}` runs the compiler frontend over one buffer and
  returns the checked package, the one entry every semantic feature
  uses.
- `ast_query.{h,cc}` names the span a declaration's name occupies,
  shared by hover and navigation.
- `completion.{h,cc}` offers the names the checked package holds, plus
  the language's keywords, filtered by the prefix the cursor typed.
- `diagnostics.{h,cc}` runs `linter::lint_source` over one buffer and
  renders the bag as LSP diagnostics, so a compiler error and a lint
  finding arrive from the same pass.
- `formatting.{h,cc}` returns the `alcy_fmt` edit that canonicalizes a
  buffer, or none when the buffer already has that layout.
- `inlay_hints.{h,cc}` shows the inferred type of a binding that wrote
  none, and the size, alignment, and field offsets of every struct or
  enum, from the checked package.
- `hover.{h,cc}` reads the checked package once and renders what the
  cursor is on: a declaration's layout or signature, a field or
  variant's place in it, and the type an expression or a written type
  resolved to.
- `layout.{h,cc}` reads a struct's or enum's size, alignment, and
  field or variant offsets from the checked package (`ir::type_layout`),
  and renders them for hover markdown and for the `alcy/structLayout`
  request.
- `navigation.{h,cc}` answers definition and references from the
  checker's recorded resolutions (ADR-0085): a use resolves to the
  declaration the checker bound, and the AST supplies a declaration's
  own name when the cursor sits on it.
- `signature.{h,cc}` reads the call the cursor sits in, follows the
  recorded target to its signature, and names the argument position.
- `protocol.{h,cc}` holds the JSON a position and a range are spelled
  as, so diagnostics, formatting, and the outline cannot disagree.
- `symbols.{h,cc}` builds the outline from the compiler's parse: the
  AST's items, with struct fields, enum variants, and impl or spec
  members as children.
- `typing.{h,cc}` renders a decided type the way source spells it.
- `server.{h,cc}` routes the methods and owns the session lifecycle;
  `lsp.{h,cc}` is the entry point the cli calls.

The unit suite drives framing, position mapping, diagnostics, the
layout engine, hover, inlay hints, navigation, completion, signature
help, the outline, and folding directly. `tools/check_lsp.py` runs a
session against the built binary and asserts the handshake, a published
diagnostic, a formatting edit, the outline, both kinds of fold, a
definition and its references, a completion and a signature, an inlay
hint, and the layout both hover and `alcy/structLayout` return, which
is the gate CI asks.

## Notes

Standard output is protocol only. A debug message belongs on standard
error, and the server writes none today.

Positions are zero-based and counted in UTF-16 code units, as the
protocol requires, while the compiler reports byte offsets.
`PositionIndex` is the single translation point, rebuilt per change
rather than incrementally. The buffer is small next to the frontend run
each change triggers, so a line-start table built with one scan is the
simple side of that trade; a future incremental index belongs behind
the same interface.

Diagnostics run the linter's single-source entry, which runs the
frontend the rules need and then the rules. Caching per-document
analysis is future work behind the same request path, not a second
analysis path.

The checked package also holds the standard library, whose spans
belong to other files. A feature compares a span against the cursor
only when its file is the buffer the request opened, or a name from
another module would answer for it.

## Plan

The features land one at a time, each reusing the parse or the analysis
an earlier one already built (ADR-0084). A syntax feature reads the
compiler's AST, a trivia feature reads `cst/`, and a semantic feature
reads the checked package through an analyzer query.

1. Outline (`documentSymbol`) and folding ranges.
2. Hover for declarations, types, and expressions, and enum layouts.
3. Definition and references over a resolver seam.
4. Completion and signature help.
5. alcy-specific inlay hints: inferred types and layouts.
6. A per-document analysis cache, so a request does not re-run the
   frontend.
7. Quick fixes for the linter's machine-applicable findings.
