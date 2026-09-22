// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"
#include "source/source.h"

namespace lexer {

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
