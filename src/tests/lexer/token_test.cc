// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "lexer/token.h"

#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace lexer {

TEST_CASE("Token carries its kind and span") {
  const diag::Span span{.file = 2, .offset = 8, .length = 3};
  const Token token{.kind = TokenKind::Fn, .span = span};
  CHECK(token.kind == TokenKind::Fn);
  CHECK(token.span.file == 2);
  CHECK(token.span.offset == 8);
  CHECK(token.span.length == 3);

  const Token eof;
  CHECK(eof.kind == TokenKind::Eof);
}

TEST_CASE("Token kinds stay within one byte") {
  CHECK(static_cast<u8>(TokenKind::Eof) < 255);
}

}  // namespace lexer
