// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"
#include "lexer/token.h"

namespace cst {

// Which comment a trivia run is.
enum class CommentKind : u8 { Line, Block, Doc };

// One entry of the lossless stream. The compiler's lexer already reads
// every token; this stream adds the comments it treats as trivia and keeps
// the newline-inserted semicolons so the layout can see where a statement
// ended. A synthetic semicolon is one the lexer inserted at a newline; an
// explicit one is spelled in the source.
struct Lexeme {
  enum class Kind : u8 { Token, Comment, End };

  Kind kind = Kind::End;
  lexer::TokenKind token = lexer::TokenKind::Eof;
  CommentKind comment = CommentKind::Line;
  u32 offset = 0;
  u32 length = 0;
  bool synthetic_semi = false;
  // Newlines in the gap before this entry, counted so a blank line is
  // distinguishable from a line break. A synthetic semicolon adds the one
  // it stands for.
  usize newlines_before = 0;
};

// Reads source into the lossless stream, ending with one End entry.
std::vector<Lexeme> lex(std::string_view source);

// The bytes a lexeme covers.
std::string_view text_of(std::string_view source, u32 offset, u32 length);

}  // namespace cst
