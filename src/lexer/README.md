# lexer

Hand-written recursive lexer over UTF-8 source bytes.

Lexing never fails hard: invalid input becomes `Error` tokens with
diagnostics in the bag, and tokenization always terminates with `Eof`.
Callers own the output buffer and may reuse it across files.

## Entry points

- `Lexer::tokenize(out)` appends the token stream, ending with `Eof`.
- `verify_token_stream(tokens, file, bytes)` ->
  `base::Result<void, TokenStreamError>`: non-empty, terminated by
  `Eof`, every span inside `bytes` for `file`. The lexer's own output
  always passes; hand-built streams must pass before reaching the
  parser.

## Input requirements

- `Lexer` takes the file's bytes and `FileId`; every emitted span
  names that file and lies within those bytes.
- `TokenKind` is frozen with `docs/spec/keywords.md`: additions
  require a specification update.
