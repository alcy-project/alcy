// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "lexer/token.h"
#include "source/source.h"

namespace lexer {

// Structural failure of a token stream handed to the parser.
enum class TokenStreamError : u8 {
  Empty,
  MissingEof,
  WrongFile,
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
  // Reports the error token the lexer produced in place of one it could
  // read. The message is a catalog key, so the wording lives in one
  // place and the code stays with the check that found it.
  template <i18n::Key K, diag::DiagnosticId Id>
  void emit_error(std::vector<Token>& out,
                  usize start,
                  usize length,
                  diag::Stage stage,
                  Id id);

  std::string_view bytes_;
  source::FileId file_;
  diag::DiagBag& bag_;
  usize pos_ = 0;
  TokenKind last_significant_ = TokenKind::Eof;
};

template <i18n::Key K, diag::DiagnosticId Id>
void Lexer::emit_error(std::vector<Token>& out,
                       usize start,
                       usize length,
                       diag::Stage stage,
                       Id id) {
  const diag::Span span = span_at(start, length);
  (void)bag_.emit<K>(diag::Severity::Error, stage, id, span);
  out.push_back(Token{.kind = TokenKind::Error, .span = span});
}

}  // namespace lexer
