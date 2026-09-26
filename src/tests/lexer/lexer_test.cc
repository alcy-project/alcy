// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lexer/lexer.h"

#include <span>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "lexer/token.h"
#include "source/source.h"

namespace lexer {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena};

  Fixture() { arena.reserve(1u << 20); }
};

std::vector<Token> lex_all(std::string_view bytes, diag::DiagBag& bag) {
  Lexer lexer(bytes, source::UNKNOWN_FILE, bag);
  std::vector<Token> out;
  lexer.tokenize(out);
  return out;
}

std::vector<TokenKind> kinds_of(const std::vector<Token>& tokens) {
  std::vector<TokenKind> kinds;
  kinds.reserve(tokens.size());
  for (const Token& token : tokens) {
    kinds.push_back(token.kind);
  }
  return kinds;
}

bool check_kinds(std::string_view bytes,
                 diag::DiagBag& bag,
                 const std::vector<TokenKind>& expected) {
  const std::vector<Token> tokens = lex_all(bytes, bag);
  const std::vector<TokenKind> actual = kinds_of(tokens);
  if (actual.size() != expected.size()) {
    INFO("input: ", bytes);
    return false;
  }
  for (usize i = 0; i < actual.size(); ++i) {
    if (actual[i] != expected[i]) {
      INFO("input: ", bytes);
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("Lexer emits nothing but Eof for empty input") {
  Fixture f;
  CHECK(check_kinds("", f.bag, {TokenKind::Eof}));
  CHECK(
      check_kinds("   \t\n  // comment\n/* block */", f.bag, {TokenKind::Eof}));
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Lexer tokenizes a small function") {
  Fixture f;
  const std::vector<Token> tokens = lex_all("fn main() {\n}", f.bag);
  CHECK(kinds_of(tokens) ==
        std::vector<TokenKind>({TokenKind::Fn, TokenKind::Ident,
                                TokenKind::LParen, TokenKind::RParen,
                                TokenKind::LBrace, TokenKind::RBrace,
                                TokenKind::Eof}));
  CHECK(!f.bag.has_errors());
  CHECK(tokens[0].span.offset == 0);
  CHECK(tokens[0].span.length == 2);
  CHECK(tokens[1].span.offset == 3);
}

TEST_CASE("Lexer inserts semicolons at newlines") {
  Fixture f;
  CHECK(check_kinds("a\nb", f.bag,
                    {TokenKind::Ident, TokenKind::Semicolon, TokenKind::Ident,
                     TokenKind::Eof}));
  CHECK(check_kinds("a();\nb();", f.bag,
                    {TokenKind::Ident, TokenKind::LParen, TokenKind::RParen,
                     TokenKind::Semicolon, TokenKind::Ident, TokenKind::LParen,
                     TokenKind::RParen, TokenKind::Semicolon, TokenKind::Eof}));
  // No insertion inside brackets or after operators.
  CHECK(check_kinds(
      "foo(a,\nb)", f.bag,
      {TokenKind::Ident, TokenKind::LParen, TokenKind::Ident, TokenKind::Comma,
       TokenKind::Ident, TokenKind::RParen, TokenKind::Eof}));
  CHECK(check_kinds(
      "a +\nb", f.bag,
      {TokenKind::Ident, TokenKind::Plus, TokenKind::Ident, TokenKind::Eof}));
  // Closing braces self-delimit: no insertion, and `else` chains.
  CHECK(check_kinds("}\nelse", f.bag,
                    {TokenKind::RBrace, TokenKind::Else, TokenKind::Eof}));
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Lexer suppresses separators before continuations") {
  Fixture f;
  // Unclosed brackets, commas, and closers continue the construct.
  CHECK(check_kinds("foo(a\n)", f.bag,
                    {TokenKind::Ident, TokenKind::LParen, TokenKind::Ident,
                     TokenKind::RParen, TokenKind::Eof}));
  // Method chains continue across lines...
  CHECK(check_kinds("foo()\n.bar()", f.bag,
                    {TokenKind::Ident, TokenKind::LParen, TokenKind::RParen,
                     TokenKind::Dot, TokenKind::Ident, TokenKind::LParen,
                     TokenKind::RParen, TokenKind::Eof}));
  // ...while a following call starts a new statement (no chain).
  CHECK(check_kinds("foo()\n(bar)", f.bag,
                    {TokenKind::Ident, TokenKind::LParen, TokenKind::RParen,
                     TokenKind::Semicolon, TokenKind::LParen, TokenKind::Ident,
                     TokenKind::RParen, TokenKind::Eof}));
  // Leading operators do not continue; trailing operators do (see above).
  CHECK(check_kinds("a\n+b", f.bag,
                    {TokenKind::Ident, TokenKind::Semicolon, TokenKind::Plus,
                     TokenKind::Ident, TokenKind::Eof}));
  // Block ends terminate their statement through insertion.
  CHECK(check_kinds(
      "if c {1}\nfoo()", f.bag,
      {TokenKind::If, TokenKind::Ident, TokenKind::LBrace, TokenKind::Integer,
       TokenKind::RBrace, TokenKind::Semicolon, TokenKind::Ident,
       TokenKind::LParen, TokenKind::RParen, TokenKind::Eof}));
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Lexer distinguishes colons equals and dots") {
  Fixture f;
  CHECK(check_kinds("a := 1", f.bag,
                    {TokenKind::Ident, TokenKind::ColonEq, TokenKind::Integer,
                     TokenKind::Eof}));
  CHECK(check_kinds("a: i32 = 1", f.bag,
                    {TokenKind::Ident, TokenKind::Colon, TokenKind::I32,
                     TokenKind::Eq, TokenKind::Integer, TokenKind::Eof}));
  CHECK(check_kinds(
      "a == b", f.bag,
      {TokenKind::Ident, TokenKind::EqEq, TokenKind::Ident, TokenKind::Eof}));
  CHECK(check_kinds("a::b", f.bag,
                    {TokenKind::Ident, TokenKind::ColonColon, TokenKind::Ident,
                     TokenKind::Eof}));
  CHECK(check_kinds("1..2", f.bag,
                    {TokenKind::Integer, TokenKind::DotDot, TokenKind::Integer,
                     TokenKind::Eof}));
  CHECK(check_kinds("1..=2", f.bag,
                    {TokenKind::Integer, TokenKind::DotDotEq,
                     TokenKind::Integer, TokenKind::Eof}));
  CHECK(check_kinds("1..<2", f.bag,
                    {TokenKind::Integer, TokenKind::DotDotLess,
                     TokenKind::Integer, TokenKind::Eof}));
  CHECK(check_kinds(
      "a -> b", f.bag,
      {TokenKind::Ident, TokenKind::Arrow, TokenKind::Ident, TokenKind::Eof}));
  CHECK(check_kinds("a => b", f.bag,
                    {TokenKind::Ident, TokenKind::FatArrow, TokenKind::Ident,
                     TokenKind::Eof}));
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Lexer reads numbers with bases and suffixes") {
  Fixture f;
  CHECK(check_kinds("42 0xFF 0b101 0o77 1_000 42i32 1.5f64", f.bag,
                    {TokenKind::Integer, TokenKind::Integer, TokenKind::Integer,
                     TokenKind::Integer, TokenKind::Integer, TokenKind::Integer,
                     TokenKind::Float, TokenKind::Eof}));
  CHECK(check_kinds(
      "1.5 1e10 1E-3", f.bag,
      {TokenKind::Float, TokenKind::Float, TokenKind::Float, TokenKind::Eof}));
  CHECK(!f.bag.has_errors());

  CHECK(check_kinds("0b102", f.bag, {TokenKind::Error, TokenKind::Eof}));
  CHECK(check_kinds("0x", f.bag, {TokenKind::Error, TokenKind::Eof}));
  CHECK(check_kinds("42_", f.bag, {TokenKind::Error, TokenKind::Eof}));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Lexer reads strings and reports unterminated ones") {
  Fixture f;
  CHECK(check_kinds(R"("hi" "a\nb" "")", f.bag,
                    {TokenKind::String, TokenKind::String, TokenKind::String,
                     TokenKind::Eof}));
  CHECK(!f.bag.has_errors());

  CHECK(check_kinds("\"oops\nnext", f.bag,
                    {TokenKind::Error, TokenKind::Ident, TokenKind::Eof}));
  CHECK(check_kinds("\"bad\\q\"", f.bag, {TokenKind::Error, TokenKind::Eof}));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Lexer reads characters") {
  Fixture f;
  CHECK(check_kinds(
      "'a' 'ab' '\\n'", f.bag,
      {TokenKind::Char, TokenKind::Char, TokenKind::Char, TokenKind::Eof}));
  CHECK(!f.bag.has_errors());

  CHECK(check_kinds("''", f.bag, {TokenKind::Error, TokenKind::Eof}));
  CHECK(check_kinds("'oops", f.bag, {TokenKind::Error, TokenKind::Eof}));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Lexer separates keywords identifiers and reserved words") {
  Fixture f;
  CHECK(check_kinds("fnx iffy _", f.bag,
                    {TokenKind::Ident, TokenKind::Ident, TokenKind::Underscore,
                     TokenKind::Eof}));
  CHECK(check_kinds("for Self package::foo", f.bag,
                    {TokenKind::For, TokenKind::SelfType, TokenKind::Package,
                     TokenKind::ColonColon, TokenKind::Ident, TokenKind::Eof}));
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Lexer handles block comments and doc comments") {
  Fixture f;
  CHECK(check_kinds("/* a /* nested */ b */ x", f.bag,
                    {TokenKind::Ident, TokenKind::Eof}));
  CHECK(check_kinds("/// doc\nx", f.bag,
                    {TokenKind::DocComment, TokenKind::Ident, TokenKind::Eof}));
  CHECK(check_kinds("//// not doc\nx", f.bag,
                    {TokenKind::Ident, TokenKind::Eof}));
  CHECK(!f.bag.has_errors());

  CHECK(check_kinds("/* oops", f.bag, {TokenKind::Error, TokenKind::Eof}));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Lexer recovers from invalid characters") {
  Fixture f;
  CHECK(check_kinds("a @ b $ c", f.bag,
                    {TokenKind::Ident, TokenKind::Error, TokenKind::Ident,
                     TokenKind::Error, TokenKind::Ident, TokenKind::Eof}));
  CHECK(f.bag.has_errors());
}

TEST_CASE("Token stream verification accepts lexer output") {
  Fixture f;
  constexpr std::string_view bytes = "fn main() {}\n";
  const std::vector<Token> tokens = lex_all(bytes, f.bag);
  CHECK(
      verify_token_stream(std::span<const Token>(tokens.data(), tokens.size()),
                          source::UNKNOWN_FILE, bytes)
          .is_ok());
}

TEST_CASE("Token stream verification rejects malformed streams") {
  Fixture f;
  constexpr std::string_view bytes = "fn main() {}\n";

  const std::vector<Token> empty;
  CHECK(verify_token_stream({}, source::UNKNOWN_FILE, bytes).is_err());

  const std::vector<Token> tokens = lex_all(bytes, f.bag);
  const std::span<const Token> all(tokens.data(), tokens.size());
  CHECK(verify_token_stream(all.subspan(0, all.size() - 1),
                            source::UNKNOWN_FILE, bytes)
            .is_err());

  Lexer other(bytes, 5, f.bag);
  std::vector<Token> foreign;
  other.tokenize(foreign);
  CHECK(verify_token_stream(
            std::span<const Token>(foreign.data(), foreign.size()),
            source::UNKNOWN_FILE, bytes)
            .is_err());

  const Token past_end{.kind = TokenKind::Eof,
                       .span = {source::UNKNOWN_FILE, 100, 1}};
  CHECK(verify_token_stream(std::span<const Token>(&past_end, 1),
                            source::UNKNOWN_FILE, bytes)
            .is_err());
}

}  // namespace lexer
