// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lexer/lexer.h"

#include <span>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lexer/token.h"
#include "source/source.h"

namespace lexer {

namespace {

// Diagnostic codes 2000-2099 are reserved for the lexer.
constexpr u32 LEXER_INVALID_CHAR = 2000;
constexpr u32 LEXER_UNTERMINATED_STRING = 2001;
constexpr u32 LEXER_UNTERMINATED_CHAR = 2002;
constexpr u32 LEXER_INVALID_NUMBER = 2003;
constexpr u32 LEXER_UNTERMINATED_BLOCK_COMMENT = 2004;
constexpr u32 LEXER_INVALID_ESCAPE = 2005;

bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

bool is_alnum_or_underscore(char c) {
  return is_alpha(c) || is_digit(c) || c == '_';
}

// A newline ends the statement started by these tokens, so the lexer
// materializes the terminating `;` (Go-style insertion), unless the
// next token continues the construct (see suppress_semi below).
// Closing braces are included: `Foo { .. }` or `if c { .. }` ending a
// line still terminate their statement. The parser needs no special
// brace handling beyond chaining `else` (covered by suppression) and
// tolerating stray separators.
bool inserts_semi(TokenKind kind) {
  switch (kind) {
    case TokenKind::Ident:
    case TokenKind::Underscore:
    case TokenKind::Integer:
    case TokenKind::Float:
    case TokenKind::String:
    case TokenKind::Char:
    case TokenKind::True:
    case TokenKind::False:
    case TokenKind::RParen:
    case TokenKind::RBracket:
    case TokenKind::RBrace:
    case TokenKind::Break:
    case TokenKind::Continue:
    case TokenKind::Ret:
    // A cast target (`1 as u64`, `x as Self`) ends the expression, so
    // type keywords terminate the statement too. `!` is excluded: a
    // trailing `!` negates and always continues the expression.
    // A postfix `?` likewise ends its expression.
    case TokenKind::Question:
    case TokenKind::Self:
    case TokenKind::SelfType:
    case TokenKind::I8:
    case TokenKind::I16:
    case TokenKind::I32:
    case TokenKind::I64:
    case TokenKind::I128:
    case TokenKind::Isize:
    case TokenKind::U8:
    case TokenKind::U16:
    case TokenKind::U32:
    case TokenKind::U64:
    case TokenKind::U128:
    case TokenKind::Usize:
    case TokenKind::F32:
    case TokenKind::F64:
    case TokenKind::Bool:
    case TokenKind::Str: return true;
    default: return false;
  }
}

// Suppression set: `else` chains, closers/commas continue enclosing
// syntax, and a leading `.` continues a method chain. Closing braces
// self-delimit on top of this: the parser ends statements at `}`.
bool suppress_semi(TokenKind next) {
  switch (next) {
    case TokenKind::Else:
    case TokenKind::Comma:
    case TokenKind::RParen:
    case TokenKind::RBracket:
    case TokenKind::RBrace:
    case TokenKind::Dot:
    case TokenKind::Eof: return true;
    default: return false;
  }
}

struct Keyword {
  std::string_view spelling;
  TokenKind kind;
};

// Linear scan: identifiers are a fraction of tokens, and the table is
// small enough that smarter lookup buys nothing measurable.
constexpr Keyword KEYWORDS[] = {
    {"Self", TokenKind::SelfType},
    {"as", TokenKind::As},
    {"async", TokenKind::Async},
    {"await", TokenKind::Await},
    {"bool", TokenKind::Bool},
    {"break", TokenKind::Break},
    {"comp", TokenKind::Comp},
    {"const", TokenKind::Const},
    {"continue", TokenKind::Continue},
    {"dyn", TokenKind::Dyn},
    {"else", TokenKind::Else},
    {"enum", TokenKind::Enum},
    {"extern", TokenKind::Extern},
    {"f32", TokenKind::F32},
    {"f64", TokenKind::F64},
    {"false", TokenKind::False},
    {"fn", TokenKind::Fn},
    {"for", TokenKind::For},
    {"i128", TokenKind::I128},
    {"i16", TokenKind::I16},
    {"i32", TokenKind::I32},
    {"i64", TokenKind::I64},
    {"i8", TokenKind::I8},
    {"if", TokenKind::If},
    {"impl", TokenKind::Impl},
    {"in", TokenKind::In},
    {"intrinsic", TokenKind::Intrinsic},
    {"isize", TokenKind::Isize},
    {"loop", TokenKind::Loop},
    {"match", TokenKind::Match},
    {"mut", TokenKind::Mut},
    {"package", TokenKind::Package},
    {"pub", TokenKind::Pub},
    {"register", TokenKind::Register},
    {"ret", TokenKind::Ret},
    {"self", TokenKind::Self},
    {"static", TokenKind::Static},
    {"str", TokenKind::Str},
    {"struct", TokenKind::Struct},
    {"super", TokenKind::Super},
    {"true", TokenKind::True},
    {"u128", TokenKind::U128},
    {"u16", TokenKind::U16},
    {"u32", TokenKind::U32},
    {"u64", TokenKind::U64},
    {"u8", TokenKind::U8},
    {"union", TokenKind::Union},
    {"unsafe", TokenKind::Unsafe},
    {"use", TokenKind::Use},
    {"usize", TokenKind::Usize},
    {"where", TokenKind::Where},
    {"while", TokenKind::While},
};

TokenKind lookup_keyword(std::string_view word) {
  for (const Keyword& keyword : KEYWORDS) {
    if (keyword.spelling == word) {
      return keyword.kind;
    }
  }
  return TokenKind::Ident;
}

bool is_valid_escape(char c) {
  switch (c) {
    case 'n':
    case 't':
    case 'r':
    case '\\':
    case '"':
    case '\'':
    case '0': return true;
    default: return false;
  }
}

bool is_hex_digit(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

Lexer::Lexer(std::string_view bytes, source::FileId file, diag::DiagBag& bag)
    : bytes_(bytes), file_(file), bag_(bag) {}

diag::Span Lexer::span_at(usize start, usize length) const {
  return diag::Span{file_, static_cast<u32>(start), static_cast<u32>(length)};
}

char Lexer::peek(usize ahead) const {
  if (pos_ + ahead >= bytes_.size()) {
    return '\0';
  }
  return bytes_[pos_ + ahead];
}

void Lexer::advance(usize count) {
  pos_ += count;
}

bool Lexer::at_end() const {
  return pos_ >= bytes_.size();
}

void Lexer::emit_error(std::vector<Token>& out,
                       usize start,
                       usize length,
                       u32 code,
                       std::string_view message) {
  const diag::Span span = span_at(start, length);
  const u32 index = bag_.emit(diag::Severity::Error, code, span, "{}", message);
  (void)index;
  out.push_back(Token{.kind = TokenKind::Error, .span = span});
}

void Lexer::skip_trivia(std::vector<Token>& out) {
  while (!at_end()) {
    const char c = peek();
    if (c == ' ' || c == '\t' || c == '\r') {
      advance();
    } else if (c == '\n') {
      const usize newline = pos_;
      advance();
      if (inserts_semi(last_significant_) && !suppress_semi(peek_next_kind())) {
        last_significant_ = TokenKind::Semicolon;
        out.push_back(
            Token{.kind = TokenKind::Semicolon, .span = span_at(newline, 1)});
      }
    } else if (c == '/' && peek(1) == '/') {
      if (peek(2) == '/' && peek(3) != '/') {
        const usize start = pos_;
        advance(3);
        while (!at_end() && peek() != '\n') {
          advance();
        }
        last_significant_ = TokenKind::DocComment;
        out.push_back(Token{.kind = TokenKind::DocComment,
                            .span = span_at(start, pos_ - start)});
      } else {
        while (!at_end() && peek() != '\n') {
          advance();
        }
      }
    } else if (c == '/' && peek(1) == '*') {
      const usize start = pos_;
      advance(2);
      u32 depth = 1;
      while (!at_end() && depth > 0) {
        if (peek() == '/' && peek(1) == '*') {
          ++depth;
          advance(2);
        } else if (peek() == '*' && peek(1) == '/') {
          --depth;
          advance(2);
        } else {
          advance();
        }
      }
      if (depth > 0) {
        emit_error(out, start, pos_ - start, LEXER_UNTERMINATED_BLOCK_COMMENT,
                   "unterminated block comment");
      }
    } else {
      break;
    }
  }
}

// Looks past whitespace and comments to classify the next significant
// token for semicolon suppression. Only the suppressed kinds are
// distinguished; everything else reports Ident.
TokenKind Lexer::peek_next_kind() const {
  usize i = pos_;
  const auto at = [&](usize k) -> char {
    return (i + k < bytes_.size()) ? bytes_[i + k] : '\0';
  };
  while (true) {
    while (i < bytes_.size() && (bytes_[i] == ' ' || bytes_[i] == '\t' ||
                                 bytes_[i] == '\r' || bytes_[i] == '\n')) {
      ++i;
    }
    if (at(0) == '/' && at(1) == '/') {
      i += 2;
      while (i < bytes_.size() && bytes_[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (at(0) == '/' && at(1) == '*') {
      i += 2;
      u32 depth = 1;
      while (i < bytes_.size() && depth > 0) {
        if (bytes_[i] == '/' && i + 1 < bytes_.size() && bytes_[i + 1] == '*') {
          ++depth;
          i += 2;
        } else if (bytes_[i] == '*' && i + 1 < bytes_.size() &&
                   bytes_[i + 1] == '/') {
          --depth;
          i += 2;
        } else {
          ++i;
        }
      }
      continue;
    }
    break;
  }
  if (i >= bytes_.size()) {
    return TokenKind::Eof;
  }
  const char c = bytes_[i];
  if (is_alpha(c) || c == '_') {
    usize end = i + 1;
    while (end < bytes_.size() && is_alnum_or_underscore(bytes_[end])) {
      ++end;
    }
    if (bytes_.substr(i, end - i) == "else") {
      return TokenKind::Else;
    }
    return TokenKind::Ident;
  }
  switch (c) {
    case ',': return TokenKind::Comma;
    case ')': return TokenKind::RParen;
    case ']': return TokenKind::RBracket;
    case '}': return TokenKind::RBrace;
    case '.': return TokenKind::Dot;
    default: return TokenKind::Ident;
  }
}

void Lexer::lex_identifier(std::vector<Token>& out) {
  const usize start = pos_;
  while (!at_end() && is_alnum_or_underscore(peek())) {
    advance();
  }
  const std::string_view word = bytes_.substr(start, pos_ - start);
  const TokenKind kind =
      word == "_" ? TokenKind::Underscore : lookup_keyword(word);
  last_significant_ = kind;
  out.push_back(Token{.kind = kind, .span = span_at(start, pos_ - start)});
}

void Lexer::lex_number(std::vector<Token>& out) {
  const usize start = pos_;
  bool is_float = false;
  if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B' || peek(1) == 'o' ||
                        peek(1) == 'O' || peek(1) == 'x' || peek(1) == 'X')) {
    const char base = peek(1);
    advance(2);
    usize digit_count = 0;
    bool trailing_underscore = false;
    while (!at_end()) {
      const char d = peek();
      const bool in_base =
          d == '_'                       ? true
          : (base == 'b' || base == 'B') ? (d == '0' || d == '1')
          : (base == 'o' || base == 'O') ? (d >= '0' && d <= '7')
                                         : is_hex_digit(d);
      if (!in_base) {
        break;
      }
      trailing_underscore = (d == '_');
      if (d != '_') {
        ++digit_count;
      }
      advance();
    }
    // A letter-starting remainder is a suffix passing through for later
    // stages ("0xFFi32"); a dangling digit is invalid ("0b102").
    // Errors consume the literal-ish tail so one Error covers the mess
    // instead of cascading into follow-on tokens.
    if (digit_count == 0) {
      while (!at_end() && is_alnum_or_underscore(peek())) {
        advance();
      }
      emit_error(out, start, pos_ - start, LEXER_INVALID_NUMBER,
                 "invalid number literal");
      return;
    }
    if (trailing_underscore && !is_alpha(peek())) {
      emit_error(out, start, pos_ - start, LEXER_INVALID_NUMBER,
                 "invalid number literal");
      return;
    }
    if (is_digit(peek())) {
      while (!at_end() && is_alnum_or_underscore(peek())) {
        advance();
      }
      emit_error(out, start, pos_ - start, LEXER_INVALID_NUMBER,
                 "invalid number literal");
      return;
    }
    while (!at_end() && is_alnum_or_underscore(peek())) {
      advance();
    }
  } else {
    while (!at_end() && (is_digit(peek()) || peek() == '_')) {
      advance();
    }
    if (peek() == '.' && peek(1) != '.') {
      is_float = true;
      advance();
      while (!at_end() && (is_digit(peek()) || peek() == '_')) {
        advance();
      }
    }
    if ((peek() == 'e' || peek() == 'E') &&
        (is_digit(peek(1)) ||
         ((peek(1) == '+' || peek(1) == '-') && is_digit(peek(2))))) {
      is_float = true;
      advance();
      if (peek() == '+' || peek() == '-') {
        advance();
      }
      while (!at_end() && (is_digit(peek()) || peek() == '_')) {
        advance();
      }
    }
    if (bytes_[pos_ - 1] == '_' && !is_alpha(peek())) {
      emit_error(out, start, pos_ - start, LEXER_INVALID_NUMBER,
                 "invalid number literal");
      return;
    }
  }
  // A suffix starts at the first letter or underscore run; unknown
  // suffixes pass through for later stages to reject.
  if (!at_end() && (is_alpha(peek()) || peek() == '_')) {
    advance();
    while (!at_end() && is_alnum_or_underscore(peek())) {
      advance();
    }
  }
  last_significant_ = is_float ? TokenKind::Float : TokenKind::Integer;
  out.push_back(
      Token{.kind = last_significant_, .span = span_at(start, pos_ - start)});
}

void Lexer::lex_string(std::vector<Token>& out) {
  const usize start = pos_;
  advance();  // Opening quote.
  bool terminated = false;
  bool bad_escape = false;
  while (!at_end()) {
    const char c = peek();
    if (c == '"') {
      terminated = true;
      advance();
      break;
    }
    if (c == '\n') {
      break;
    }
    if (c == '\\') {
      if (peek(1) == '\n') {
        break;
      }
      if (peek(1) == 'u') {
        usize i = pos_ + 2;
        if (i < bytes_.size() && bytes_[i] == '{') {
          ++i;
          const usize hex = i;
          while (i < bytes_.size() && is_hex_digit(bytes_[i])) {
            ++i;
          }
          if (i != hex && i < bytes_.size() && bytes_[i] == '}') {
            pos_ = i + 1;
            continue;
          }
        }
        bad_escape = true;
        advance(2);
        continue;
      }
      if (!is_valid_escape(peek(1))) {
        bad_escape = true;
        advance(2);
        continue;
      }
      advance(2);
      continue;
    }
    advance();
  }
  if (!terminated) {
    emit_error(out, start, pos_ - start, LEXER_UNTERMINATED_STRING,
               "unterminated string literal");
    return;
  }
  if (bad_escape) {
    emit_error(out, start, pos_ - start, LEXER_INVALID_ESCAPE,
               "invalid escape in string literal");
    return;
  }
  last_significant_ = TokenKind::String;
  out.push_back(
      Token{.kind = TokenKind::String, .span = span_at(start, pos_ - start)});
}

void Lexer::lex_char(std::vector<Token>& out) {
  const usize start = pos_;
  advance();  // Opening quote.
  bool terminated = false;
  bool empty = true;
  bool bad_escape = false;
  while (!at_end()) {
    const char c = peek();
    if (c == '\'') {
      terminated = true;
      advance();
      break;
    }
    if (c == '\n') {
      break;
    }
    if (c == '\\') {
      if (peek(1) == '\n') {
        break;
      }
      if (peek(1) == 'u') {
        usize i = pos_ + 2;
        if (i < bytes_.size() && bytes_[i] == '{') {
          ++i;
          const usize hex = i;
          while (i < bytes_.size() && is_hex_digit(bytes_[i])) {
            ++i;
          }
          if (i != hex && i < bytes_.size() && bytes_[i] == '}') {
            pos_ = i + 1;
            empty = false;
            continue;
          }
        }
        bad_escape = true;
        empty = false;
        advance(2);
        continue;
      }
      if (!is_valid_escape(peek(1))) {
        bad_escape = true;
        empty = false;
        advance(2);
        continue;
      }
      advance(2);
      empty = false;
      continue;
    }
    advance();
    empty = false;
  }
  if (!terminated) {
    emit_error(out, start, pos_ - start, LEXER_UNTERMINATED_CHAR,
               "unterminated character literal");
    return;
  }
  if (empty) {
    emit_error(out, start, pos_ - start, LEXER_INVALID_CHAR,
               "empty character literal");
    return;
  }
  if (bad_escape) {
    emit_error(out, start, pos_ - start, LEXER_INVALID_ESCAPE,
               "invalid escape in character literal");
    return;
  }
  last_significant_ = TokenKind::Char;
  out.push_back(
      Token{.kind = TokenKind::Char, .span = span_at(start, pos_ - start)});
}

void Lexer::lex_symbol(std::vector<Token>& out) {
  const usize start = pos_;
  const char c = peek();
  const char n = peek(1);
  const char m = peek(2);
  TokenKind kind = TokenKind::Error;
  usize width = 1;
  switch (c) {
    case '(': kind = TokenKind::LParen; break;
    case ')': kind = TokenKind::RParen; break;
    case '{': kind = TokenKind::LBrace; break;
    case '}': kind = TokenKind::RBrace; break;
    case '[': kind = TokenKind::LBracket; break;
    case ']': kind = TokenKind::RBracket; break;
    case ',': kind = TokenKind::Comma; break;
    case ';': kind = TokenKind::Semicolon; break;
    case '?': kind = TokenKind::Question; break;
    case '#': kind = TokenKind::Hash; break;
    case '~': kind = TokenKind::Tilde; break;
    case '.':
      if (n == '.' && m == '=') {
        kind = TokenKind::DotDotEq;
        width = 3;
      } else if (n == '.' && m == '<') {
        kind = TokenKind::DotDotLess;
        width = 3;
      } else if (n == '.') {
        kind = TokenKind::DotDot;
        width = 2;
      } else {
        kind = TokenKind::Dot;
      }
      break;
    case ':':
      if (n == ':') {
        kind = TokenKind::ColonColon;
        width = 2;
      } else if (n == '=') {
        kind = TokenKind::ColonEq;
        width = 2;
      } else {
        kind = TokenKind::Colon;
      }
      break;
    case '=':
      if (n == '=') {
        kind = TokenKind::EqEq;
        width = 2;
      } else if (n == '>') {
        kind = TokenKind::FatArrow;
        width = 2;
      } else {
        kind = TokenKind::Eq;
      }
      break;
    case '!':
      kind = (n == '=') ? TokenKind::BangEq : TokenKind::Bang;
      width = (n == '=') ? 2 : 1;
      break;
    case '>':
      if (n == '>' && m == '=') {
        kind = TokenKind::GreaterGreaterEq;
        width = 3;
      } else if (n == '>') {
        kind = TokenKind::GreaterGreater;
        width = 2;
      } else if (n == '=') {
        kind = TokenKind::GreaterEq;
        width = 2;
      } else {
        kind = TokenKind::Greater;
      }
      break;
    case '<':
      if (n == '<' && m == '=') {
        kind = TokenKind::LessLessEq;
        width = 3;
      } else if (n == '<') {
        kind = TokenKind::LessLess;
        width = 2;
      } else if (n == '=') {
        kind = TokenKind::LessEq;
        width = 2;
      } else {
        kind = TokenKind::Less;
      }
      break;
    case '+':
      kind = (n == '=') ? TokenKind::PlusEq : TokenKind::Plus;
      width = (n == '=') ? 2 : 1;
      break;
    case '-':
      if (n == '=') {
        kind = TokenKind::MinusEq;
        width = 2;
      } else if (n == '>') {
        kind = TokenKind::Arrow;
        width = 2;
      } else {
        kind = TokenKind::Minus;
      }
      break;
    case '*':
      if (n == '*' && m == '=') {
        kind = TokenKind::StarStarEq;
        width = 3;
      } else if (n == '*') {
        kind = TokenKind::StarStar;
        width = 2;
      } else if (n == '=') {
        kind = TokenKind::StarEq;
        width = 2;
      } else {
        kind = TokenKind::Star;
      }
      break;
    case '/':
      kind = (n == '=') ? TokenKind::SlashEq : TokenKind::Slash;
      width = (n == '=') ? 2 : 1;
      break;
    case '%':
      kind = (n == '=') ? TokenKind::PercentEq : TokenKind::Percent;
      width = (n == '=') ? 2 : 1;
      break;
    case '&':
      if (n == '&') {
        kind = TokenKind::AmpAmp;
        width = 2;
      } else if (n == '=') {
        kind = TokenKind::AmpEq;
        width = 2;
      } else {
        kind = TokenKind::Amp;
      }
      break;
    case '|':
      if (n == '|') {
        kind = TokenKind::PipePipe;
        width = 2;
      } else if (n == '=') {
        kind = TokenKind::PipeEq;
        width = 2;
      } else {
        kind = TokenKind::Pipe;
      }
      break;
    case '^':
      kind = (n == '=') ? TokenKind::CaretEq : TokenKind::Caret;
      width = (n == '=') ? 2 : 1;
      break;
    default: break;
  }
  if (kind == TokenKind::Error) {
    // Invalid character (including '@', '$', backtick, stray UTF-8
    // bytes, and control characters): identifiers stay ASCII-only in
    // MVP, and every other stray scalar is diagnosed here.
    usize scalar = 1;
    const unsigned char lead = static_cast<unsigned char>(c);
    if ((lead & 0xE0) == 0xC0) {
      scalar = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      scalar = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      scalar = 4;
    }
    if (pos_ + scalar > bytes_.size()) {
      scalar = bytes_.size() - pos_;
    }
    emit_error(out, start, scalar, LEXER_INVALID_CHAR, "invalid character");
    advance(scalar);
    return;
  }
  advance(width);
  last_significant_ = kind;
  out.push_back(Token{.kind = kind, .span = span_at(start, width)});
}

void Lexer::tokenize(std::vector<Token>& out) {
  while (true) {
    skip_trivia(out);
    if (at_end()) {
      break;
    }
    const char c = peek();
    if (c == '\0') {
      emit_error(out, pos_, 1, LEXER_INVALID_CHAR, "invalid character");
      advance();
    } else if (is_alpha(c) || c == '_') {
      lex_identifier(out);
    } else if (is_digit(c)) {
      lex_number(out);
    } else if (c == '"') {
      lex_string(out);
    } else if (c == '\'') {
      lex_char(out);
    } else {
      lex_symbol(out);
    }
  }
  last_significant_ = TokenKind::Eof;
  out.push_back(Token{.kind = TokenKind::Eof, .span = span_at(pos_, 0)});
}

base::Result<void, TokenStreamError> verify_token_stream(
    std::span<const Token> tokens,
    source::FileId file,
    std::string_view bytes) {
  if (tokens.empty()) {
    return base::make_err(TokenStreamError::Empty);
  }
  if (tokens.back().kind != TokenKind::Eof) {
    return base::make_err(TokenStreamError::MissingEof);
  }
  const usize size = bytes.size();
  for (const Token& token : tokens) {
    if (token.span.file != file) {
      return base::make_err(TokenStreamError::WrongFile);
    }
    const usize offset = token.span.offset;
    const usize length = token.span.length;
    if (offset > size || length > size - offset) {
      return base::make_err(TokenStreamError::SpanOutOfRange);
    }
  }
  return base::make_ok();
}

std::string_view describe_token_stream_error(TokenStreamError error) {
  switch (error) {
    case TokenStreamError::Empty: return "token stream is empty";
    case TokenStreamError::MissingEof:
      return "token stream is not terminated by Eof";
    case TokenStreamError::WrongFile:
      return "token span names a different file";
    case TokenStreamError::SpanOutOfRange:
      return "token span runs past the end of the source bytes";
  }
  return "invalid token stream";
}

}  // namespace lexer
