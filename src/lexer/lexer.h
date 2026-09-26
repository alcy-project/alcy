// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lexer/token.h"
#include "source/source.h"

namespace lexer {

// Structural failure of a token stream handed to the parser.
enum class TokenStreamError : u8 {
  // No tokens at all.
  Empty,
  // The last token is not Eof.
  MissingEof,
  // A token names a different file than the bytes it is checked against.
  WrongFile,
  // A token span runs past the end of the source bytes.
  SpanOutOfRange,
};

// Pure verifier for a token stream handed to the parser: non-empty,
// terminated by Eof, and every span inside `bytes` for `file`. The
// lexer's own output satisfies this; hand-built streams must pass it
// first. No I/O, no logging, no bag writes.
base::Result<void, TokenStreamError> verify_token_stream(
    std::span<const Token> tokens,
    source::FileId file,
    std::string_view bytes);

// Short human-readable detail for a token stream failure.
std::string_view describe_token_stream_error(TokenStreamError error);

// Hand-written recursive lexer over UTF-8 source bytes. Lexing never
// fails hard: invalid input becomes Error tokens with diagnostics in
// `bag`, and tokenization always terminates with Eof. Callers own the
// output buffer and may reuse it across files.
class Lexer {
 public:
  Lexer(std::string_view bytes, source::FileId file, diag::DiagBag& bag);

  // Appends the token stream for the input, ending with Eof.
  void tokenize(std::vector<Token>& out);

 private:
  diag::Span span_at(usize start, usize length) const;
  char peek(usize ahead = 0) const;
  void advance(usize count = 1);
  bool at_end() const;

  void skip_trivia(std::vector<Token>& out);
  TokenKind peek_next_kind() const;
  void lex_identifier(std::vector<Token>& out);
  void lex_number(std::vector<Token>& out);
  void lex_string(std::vector<Token>& out);
  void lex_char(std::vector<Token>& out);
  void lex_symbol(std::vector<Token>& out);
  void emit_error(std::vector<Token>& out,
                  usize start,
                  usize length,
                  u32 code,
                  std::string_view message);

  std::string_view bytes_;
  source::FileId file_;
  diag::DiagBag& bag_;
  usize pos_ = 0;
  TokenKind last_significant_ = TokenKind::Eof;
};

}  // namespace lexer
