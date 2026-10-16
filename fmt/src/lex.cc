// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/src/lex.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "source/source.h"

namespace alcy_fmt {

namespace {

constexpr usize NEWLINE_CAP = 2;

void add_newlines(usize& pending, usize count) {
  pending = std::min(pending + count, NEWLINE_CAP);
}

void push_comment(std::vector<Lexeme>& out,
                  usize& pending,
                  CommentKind kind,
                  u32 offset,
                  u32 length) {
  Lexeme lexeme;
  lexeme.kind = Lexeme::Kind::Comment;
  lexeme.comment = kind;
  lexeme.offset = offset;
  lexeme.length = length;
  lexeme.newlines_before = pending;
  out.push_back(lexeme);
  pending = 0;
}

// Scans the bytes the compiler's lexer left as trivia. They are whitespace
// and comments by construction; a byte that is neither is skipped rather
// than allowed to stall the walk.
void scan_gap(std::string_view source,
              usize begin,
              usize end,
              usize& pending,
              std::vector<Lexeme>& out) {
  usize i = begin;
  while (i < end) {
    const char c = source[i];
    if (c == '\n') {
      add_newlines(pending, 1);
      ++i;
    } else if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
    } else if (c == '/' && i + 1 < end && source[i + 1] == '/') {
      const usize start = i;
      i += 2;
      while (i < end && source[i] != '\n') {
        ++i;
      }
      push_comment(out, pending, CommentKind::Line, static_cast<u32>(start),
                   static_cast<u32>(i - start));
    } else if (c == '/' && i + 1 < end && source[i + 1] == '*') {
      const usize start = i;
      i += 2;
      u32 depth = 1;
      while (i < end && depth > 0) {
        if (i + 1 < end && source[i] == '/' && source[i + 1] == '*') {
          ++depth;
          i += 2;
        } else if (i + 1 < end && source[i] == '*' && source[i + 1] == '/') {
          --depth;
          i += 2;
        } else {
          ++i;
        }
      }
      push_comment(out, pending, CommentKind::Block, static_cast<u32>(start),
                   static_cast<u32>(i - start));
    } else {
      ++i;
    }
  }
}

}  // namespace

std::vector<Lexeme> lex(std::string_view source) {
  mem::Arena arena;
  arena.reserve(1u << 16);
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  lexer::Lexer lexer(source, source::UNKNOWN_FILE, bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);

  std::vector<Lexeme> out;
  usize cursor = 0;
  usize pending = 0;
  for (const lexer::Token& token : tokens) {
    const usize start = cursor;
    const usize end = std::min<usize>(token.span.offset, source.size());
    if (start < end) {
      scan_gap(source, start, end, pending, out);
    }
    cursor = std::min<usize>(
        static_cast<usize>(token.span.offset) + token.span.length,
        source.size());

    if (token.kind == lexer::TokenKind::Eof) {
      Lexeme lexeme;
      lexeme.kind = Lexeme::Kind::End;
      lexeme.offset = static_cast<u32>(cursor);
      lexeme.length = 0;
      out.push_back(lexeme);
      break;
    }
    if (token.kind == lexer::TokenKind::DocComment) {
      push_comment(out, pending, CommentKind::Doc, token.span.offset,
                   token.span.length);
      continue;
    }

    Lexeme lexeme;
    lexeme.kind = Lexeme::Kind::Token;
    lexeme.token = token.kind;
    lexeme.offset = token.span.offset;
    lexeme.length = token.span.length;
    lexeme.newlines_before = pending;
    pending = 0;
    if (token.kind == lexer::TokenKind::Semicolon) {
      const bool spelled =
          token.span.offset < source.size() &&
          source[token.span.offset] == ';';
      lexeme.synthetic_semi = !spelled;
      if (lexeme.synthetic_semi) {
        // The inserted semicolon stands for the newline it replaced.
        pending = 1;
      }
    }
    out.push_back(lexeme);
  }
  if (out.empty() || out.back().kind != Lexeme::Kind::End) {
    Lexeme lexeme;
    lexeme.kind = Lexeme::Kind::End;
    lexeme.offset = static_cast<u32>(source.size());
    lexeme.length = 0;
    out.push_back(lexeme);
  }
  return out;
}

std::string_view text_of(std::string_view source, u32 offset, u32 length) {
  if (offset >= source.size()) {
    return {};
  }
  const usize available = source.size() - offset;
  const usize size = std::min<usize>(length, available);
  return source.substr(offset, size);
}

}  // namespace alcy_fmt
