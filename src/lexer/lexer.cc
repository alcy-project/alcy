// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lexer/lexer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "lexer/diag_code.h"
#include "lexer/token.h"
#include "source/source.h"

namespace lexer {

namespace {

// Identifier and number bodies are most of a source file, so the byte
// test is one table load rather than four range comparisons.
constexpr std::array<bool, 256> ALNUM_OR_UNDERSCORE = [] {
  std::array<bool, 256> table{};
  for (u32 c = '0'; c <= '9'; ++c) {
    table[c] = true;
  }
  for (u32 c = 'a'; c <= 'z'; ++c) {
    table[c] = true;
  }
  for (u32 c = 'A'; c <= 'Z'; ++c) {
    table[c] = true;
  }
  table[static_cast<u32>('_')] = true;
  return table;
}();

bool is_alnum_or_underscore(char c) {
  return ALNUM_OR_UNDERSCORE[static_cast<u8>(c)];
}

bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

// A newline ends the statement started by these tokens, so the lexer
// materializes the terminating `;` (Go-style insertion), unless the
// next token continues the construct (see suppress_semi below).
// Closing braces are included: `Foo { .. }` or `if c { .. }` ending a
// line still terminate their statement.
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
    // A range operator can also end one: `n := 0..` is a complete
    // value, so the following newline terminates it.
    case TokenKind::Question:
    case TokenKind::DotDot:
    case TokenKind::DotDotEq:
    case TokenKind::DotDotLess:
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

// One entry per keyword, in spelling order for reading; the lookup packs
// them into a keyed table at compile time.
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
    {"spec", TokenKind::Spec},
    {"static", TokenKind::Static},
    {"str", TokenKind::Str},
    {"struct", TokenKind::Struct},
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

constexpr usize KEYWORD_MIN_LENGTH = [] {
  usize shortest = KEYWORDS[0].spelling.size();
  for (const Keyword& keyword : KEYWORDS) {
    shortest = std::min(shortest, keyword.spelling.size());
  }
  return shortest;
}();
constexpr usize KEYWORD_MAX_LENGTH = [] {
  usize longest = KEYWORDS[0].spelling.size();
  for (const Keyword& keyword : KEYWORDS) {
    longest = std::max(longest, keyword.spelling.size());
  }
  return longest;
}();
// A spelling's opening bytes, packed low byte first. Every keyword but
// `intrinsic` fits in eight, so one integer and a length settle a word
// against a spelling without walking its bytes.
constexpr usize SPELLING_KEY_BYTES = 8;

constexpr u64 packed_spelling_key(const char* word, usize packed) {
  u64 key = 0;
  for (usize i = 0; i < packed; ++i) {
    key |= static_cast<u64>(static_cast<u8>(word[i])) << (8 * i);
  }
  return key;
}

// The key is packed low byte first, so a big-endian host reverses the load
// that produced it.
[[maybe_unused]] constexpr u64 swap_key_bytes(u64 key) {
  u64 swapped = 0;
  for (usize i = 0; i < SPELLING_KEY_BYTES; ++i) {
    swapped |= ((key >> (8 * i)) & 0xFF) << (8 * (SPELLING_KEY_BYTES - 1 - i));
  }
  return swapped;
}

// `available` is how many bytes of `word` the file holds. While a whole key
// is readable the opening bytes arrive in one load, which the mask trims to
// the word's length; the file's last few bytes are packed one at a time.
u64 spelling_key(const char* word, usize length, usize available) {
  const usize packed = std::min(length, SPELLING_KEY_BYTES);
  if (available < SPELLING_KEY_BYTES) {
    return packed_spelling_key(word, packed);
  }
  u64 key = 0;
  std::memcpy(&key, word, SPELLING_KEY_BYTES);
  if constexpr (std::endian::native == std::endian::big) {
    key = swap_key_bytes(key);
  }
  if (packed < SPELLING_KEY_BYTES) {
    key &= (u64{1} << (8 * packed)) - 1;
  }
  return key;
}

// A word longer than a key carries the key of the bytes it opens with, so a
// slot also records the length of the spelling it holds.
struct KeywordSlot {
  u64 key = 0;
  TokenKind kind = TokenKind::Ident;
  u8 length = 0;
};

// Fifty-two spellings sit in a table twice that size, so the home slot
// answers most words that are one and the walk that follows stays short.
constexpr usize KEYWORD_KEY_BITS = 7;
constexpr usize KEYWORD_SLOTS = 1 << KEYWORD_KEY_BITS;

constexpr usize keyword_home(u64 key) {
  return static_cast<usize>((key * 0x9E3779B97F4A7C15ULL) >>
                            (64 - KEYWORD_KEY_BITS));
}

constexpr std::array<KeywordSlot, KEYWORD_SLOTS> keyword_table() {
  std::array<KeywordSlot, KEYWORD_SLOTS> table{};
  for (const Keyword& keyword : KEYWORDS) {
    const usize packed = std::min(keyword.spelling.size(), SPELLING_KEY_BYTES);
    const KeywordSlot slot{
        .key = packed_spelling_key(keyword.spelling.data(), packed),
        .kind = keyword.kind,
        .length = static_cast<u8>(keyword.spelling.size())};
    usize home = keyword_home(slot.key);
    while (table[home].key != 0) {
      home = (home + 1) & (KEYWORD_SLOTS - 1);
    }
    table[home] = slot;
  }
  return table;
}

// Two spellings sharing a key and a length would be the same word, so each
// one is reachable on its own.
constexpr bool keyword_keys_distinct() {
  for (usize i = 0; i < std::size(KEYWORDS); ++i) {
    for (usize j = i + 1; j < std::size(KEYWORDS); ++j) {
      const usize packed =
          std::min(KEYWORDS[i].spelling.size(), SPELLING_KEY_BYTES);
      if (KEYWORDS[i].spelling.size() == KEYWORDS[j].spelling.size() &&
          packed_spelling_key(KEYWORDS[i].spelling.data(), packed) ==
              packed_spelling_key(KEYWORDS[j].spelling.data(), packed)) {
        return false;
      }
    }
  }
  return true;
}

static_assert(keyword_keys_distinct());

constexpr auto KEYWORD_TABLE = keyword_table();

// `key` is `spelling_key` of the word's opening bytes. Every identifier and
// every keyword goes through here, so the comparison it makes is the lexer's
// most repeated one.
TokenKind lookup_keyword(usize length, u64 key) {
  if (length < KEYWORD_MIN_LENGTH || length > KEYWORD_MAX_LENGTH) {
    return TokenKind::Ident;
  }
  usize home = keyword_home(key);
  while (true) {
    const KeywordSlot& slot = KEYWORD_TABLE[home];
    if (slot.key == 0) {
      return TokenKind::Ident;
    }
    if (slot.key == key && slot.length == length) {
      return slot.kind;
    }
    home = (home + 1) & (KEYWORD_SLOTS - 1);
  }
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

u32 hex_digit_value(char c) {
  if (is_digit(c)) {
    return static_cast<u32>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<u32>(c - 'a' + 10);
  }
  return static_cast<u32>(c - 'A' + 10);
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
        // A comment is trivia: it does not become the token a newline
        // tests for semi insertion, or a `///` would swallow the
        // statement boundary a `//` leaves.
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
        emit_error<i18n::Key::LexerUnterminatedBlockComment>(
            out, start, pos_ - start, DiagCode::UnterminatedBlockComment);
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
      // A block comment the source never closed is a token the real
      // lexer reports, not trivia that ends the file: the suppression
      // test has to see it, or a statement the newline terminates
      // loses the `;`, and the layout reads the gap differently on the
      // pass over its own output.
      if (depth > 0) {
        return TokenKind::Error;
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
  const char* const data = bytes_.data();
  const usize size = bytes_.size();
  usize end = start;
  while (end < size && is_alnum_or_underscore(data[end])) {
    ++end;
  }
  pos_ = end;
  const usize length = pos_ - start;
  const TokenKind kind =
      length == 1 && data[start] == '_'
          ? TokenKind::Underscore
          : lookup_keyword(length,
                           spelling_key(data + start, length, size - start));
  last_significant_ = kind;
  out.push_back(Token{.kind = kind, .span = span_at(start, length)});
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
      emit_error<i18n::Key::LexerInvalidNumber>(out, start, pos_ - start,
                                                DiagCode::InvalidNumber);
      return;
    }
    if (trailing_underscore && !is_alpha(peek())) {
      emit_error<i18n::Key::LexerInvalidNumber>(out, start, pos_ - start,
                                                DiagCode::InvalidNumber);
      return;
    }
    if (is_digit(peek())) {
      while (!at_end() && is_alnum_or_underscore(peek())) {
        advance();
      }
      emit_error<i18n::Key::LexerInvalidNumber>(out, start, pos_ - start,
                                                DiagCode::InvalidNumber);
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
      // A `.` is a decimal point when a digit follows it, when what
      // follows is the float suffix itself, or when nothing that
      // could name a member follows at all. A member name after a
      // tuple index (`t.0.len()`) leaves the `.` to the parser.
      const bool ident_after = is_alpha(peek(1)) || peek(1) == '_';
      // The only member names a float suffix can be are `f32` and
      // `f64`; any other name after the point is a member.
      const bool f32 = peek(1) == 'f' && peek(2) == '3' && peek(3) == '2';
      const bool f64 = peek(1) == 'f' && peek(2) == '6' && peek(3) == '4';
      const bool suffix_after =
          (f32 || f64) && !is_alnum_or_underscore(peek(4));
      if (is_digit(peek(1)) || suffix_after || !ident_after) {
        is_float = true;
        advance();
        while (!at_end() && (is_digit(peek()) || peek() == '_')) {
          advance();
        }
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
      emit_error<i18n::Key::LexerInvalidNumber>(out, start, pos_ - start,
                                                DiagCode::InvalidNumber);
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
          u64 code = 0;
          while (i < bytes_.size() && is_hex_digit(bytes_[i])) {
            // Stop tracking once the value cannot be a scalar: the
            // answer cannot change, and the multiply would wrap.
            if (code <= 0x10FFFF) {
              code = code * 16 + hex_digit_value(bytes_[i]);
            }
            ++i;
          }
          if (i != hex && i < bytes_.size() && bytes_[i] == '}') {
            // The digits are well formed, but what they spell may not
            // be a Unicode scalar value, and unescaping would drop it -
            // a string that lost a character without a word.
            if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
              // The escape is inside a string, and the string token
              // already covers it: emitting an error token as well
              // splits the string in the lossless front end, and the
              // formatter then rewrites it into something else.
              const diag::Span span = span_at(pos_, i + 1 - pos_);
              (void)bag_.emit<i18n::Key::LexerUnicodeEscapeNotScalar>(
                  DiagCode::InvalidEscape, span);
            }
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
    emit_error<i18n::Key::LexerUnterminatedString>(
        out, start, pos_ - start, DiagCode::UnterminatedString);
    return;
  }
  if (bad_escape) {
    emit_error<i18n::Key::LexerInvalidEscapeInString>(out, start, pos_ - start,
                                                      DiagCode::InvalidEscape);
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
    emit_error<i18n::Key::LexerUnterminatedCharacter>(
        out, start, pos_ - start, DiagCode::UnterminatedChar);
    return;
  }
  if (empty) {
    emit_error<i18n::Key::LexerEmptyCharacter>(out, start, pos_ - start,
                                               DiagCode::InvalidChar);
    return;
  }
  if (bad_escape) {
    emit_error<i18n::Key::LexerInvalidEscapeInCharacter>(
        out, start, pos_ - start, DiagCode::InvalidEscape);
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
    // bytes, and control characters): identifiers stay ASCII-only, and
    // every other stray scalar is diagnosed here. A UTF-8
    // lead takes the continuation bytes that follow it and stops at
    // the first byte that is neither one nor part of the promise: a
    // span that ran over whitespace would read back longer than the
    // layout wrote, and a formatter over its own output would disagree
    // with the formatter over the source.
    usize scalar = 1;
    const u8 lead = static_cast<u8>(c);
    usize promise = 1;
    if ((lead & 0xE0) == 0xC0) {
      promise = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      promise = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      promise = 4;
    }
    while (scalar < promise && pos_ + scalar < bytes_.size() &&
           (static_cast<u8>(bytes_[pos_ + scalar]) & 0xC0) == 0x80) {
      ++scalar;
    }
    if (pos_ + scalar > bytes_.size()) {
      scalar = bytes_.size() - pos_;
    }
    emit_error<i18n::Key::LexerInvalidCharacter>(out, start, scalar,
                                                 DiagCode::InvalidChar);
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
      emit_error<i18n::Key::LexerInvalidCharacter>(out, pos_, 1,
                                                   DiagCode::InvalidChar);
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
