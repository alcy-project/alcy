# parser

Hand-written recursive-descent parser over a token stream, plus the
shadowing desugar.

Parsing is error-tolerant: failures report a diagnostic and
synchronize at item, statement, or arm boundaries, so one bad
construct never hides the rest of the file. The token cursor
transparently skips lexer error tokens (already diagnosed), doc
comments, and reserved words (diagnosed here with guidance); the
grammar never observes them.

## Entry points

- `Parser::parse()` ->
  `base::Result<std::span<const ast::ItemIdx>, diag::Reported>`. The
  token stream is verified on entry (`PARSER_INVALID_TOKEN_STREAM`)
  and the arena on exit (`PARSER_INVALID_AST`); grammar errors still
  accumulate in the bag with the valid items returned.
- `desugar_shadowing(items, ast, bag)` ->
  `base::Result<void, diag::Reported>`. Freshens shadowed bindings;
  verifies the arena on exit like `parse()`.

## Input requirements

- The token stream must pass `lexer::verify_token_stream` for the
  parsed file: non-empty, `Eof`-terminated, spans inside the file's
  bytes. Anything else is rejected before the cursor moves.
- The arena holds indices, not pointers: passes never downcast, and
  every child index is either invalid (an allowed absent edge) or in
  range (`ast::verify_file`).
