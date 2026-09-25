// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lexer/token.h"
#include "source/source.h"

namespace parser {

// Diagnostic codes 4100-4199 are reserved for the parser.
inline constexpr u32 kParserUnexpectedToken = 4100;
inline constexpr u32 kParserReservedWord = 4101;

// Hand-written recursive-descent parser over a token stream. Parsing is
// error-tolerant: failures report a diagnostic and synchronize at item,
// statement, or arm boundaries, so one bad construct never hides the
// rest of the file. The token cursor transparently skips lexer error
// tokens (already diagnosed), doc comments, and reserved words (diagnosed
// here with guidance); the grammar never observes them.
class Parser {
 public:
  Parser(std::span<const lexer::Token> tokens,
         std::string_view bytes,
         source::FileId file,
         ast::AstArena& arena,
         diag::DiagBag& bag);

  // Parses a whole file into items. Always returns; errors accumulate
  // in the bag and erroneous constructs are simply absent.
  std::span<const ast::ItemIdx> parse();

 private:
  // Token cursor. Never rests on skipped kinds.
  bool at_end() const;
  lexer::TokenKind peek_kind() const;
  lexer::Token peek() const;
  lexer::Token previous() const;
  void advance();
  void skip_insignificant();
  bool check(lexer::TokenKind kind) const;
  bool match(lexer::TokenKind kind);
  bool expect(lexer::TokenKind kind, std::string_view what);
  diag::Span span_from(usize mark) const;

  // Error recovery: skips to a boundary token, tracking nesting so
  // inner closers never terminate the skip. Always consumes at least
  // one token, so recovery loops terminate.
  void synchronize();

  // Declaration vs reassignment vs expression disambiguation: scans for
  // ":="/"=" at bracket depth zero. Stops at block opens and
  // terminators without consuming.
  enum class StmtLead : u8 { None, Decl, Reassign };
  StmtLead scan_lead() const;

  // Items.
  ast::ItemIdx parse_item();
  ast::ItemIdx parse_fn(bool is_pub);
  ast::ItemIdx parse_intrinsic_fn(bool is_pub);
  bool parse_fn_params(std::vector<ast::ItemFnParam>& params);
  ast::ItemIdx parse_struct(bool is_pub);
  ast::ItemIdx parse_enum(bool is_pub);
  ast::ItemIdx parse_impl(bool is_pub);
  // Parses an optional `<T, ...>` parameter list; empty when absent.
  bool parse_generic_params(std::vector<ast::Ident>& params);
  ast::ItemIdx parse_static(bool is_pub);
  ast::ItemIdx parse_const(bool is_pub);
  ast::ItemIdx parse_use(bool is_pub);

  // Types. parse_closed_type additionally rejects a dangling banked
  // ">" left over from splitting ">>" in nested argument lists.
  ast::TypeIdx parse_type();
  ast::TypeIdx parse_closed_type();

  // Patterns and expressions (null on error, no recovery inside).
  ast::PatternIdx parse_pattern();
  ast::PatternIdx parse_or_pattern();
  ast::PatternIdx parse_primary_pattern();
  ast::ExprIdx parse_expr();
  ast::ExprIdx parse_range();
  ast::ExprIdx parse_or();
  ast::ExprIdx parse_and();
  ast::ExprIdx parse_cmp();
  ast::ExprIdx parse_bitor();
  ast::ExprIdx parse_bitxor();
  ast::ExprIdx parse_bitand();
  ast::ExprIdx parse_shift();
  ast::ExprIdx parse_add();
  ast::ExprIdx parse_mul();
  ast::ExprIdx parse_pow();
  ast::ExprIdx parse_cast();
  ast::ExprIdx parse_unary();
  ast::ExprIdx parse_postfix();
  ast::ExprIdx parse_primary();
  ast::ExprIdx parse_if();
  ast::ExprIdx parse_match();
  ast::ExprIdx parse_loop();
  ast::ExprIdx parse_while();
  ast::ExprIdx parse_block_expr();
  ast::ExprIdx parse_comp_block();
  ast::ExprIdx parse_array_literal();
  ast::BlockIdx parse_block();

  // Statements (recover at boundaries).
  ast::StmtIdx parse_stmt();

  // Small pieces.
  base::Result<ast::Ident, diag::Fatal> parse_ident(std::string_view what);
  // Parses a decimal integer literal (digits and `_`) for array
  // lengths; the token must already be checked as Integer.
  bool parse_decimal_u64(u64* out);
  // Parses a dotted path. When `type_args` is given, a `::<...>` inside
  // the path is read as a turbofish and the path continues past it.
  ast::PathIdx parse_path(std::vector<ast::TypeIdx>* type_args = nullptr);
  // Reads a turbofish argument list; `<` and its `::` are consumed.
  bool parse_turbofish(std::vector<ast::TypeIdx>& type_args);
  ast::CondIdx parse_cond();
  bool consume_gt();

  std::span<const lexer::Token> tokens_;
  std::string_view bytes_;
  source::FileId file_;
  ast::AstArena& ast_;
  diag::DiagBag& bag_;
  usize pos_ = 0;
  diag::Span previous_span_;
  // Banked ">" halves from splitting ">>" in nested type arguments.
  u32 banked_gt_ = 0;
  // Struct literals are disabled in scrutinee and condition positions
  // so their "{" reads as the body block (parenthesize to force a
  // struct there).
  bool allow_struct_lit_ = true;
};

}  // namespace parser
