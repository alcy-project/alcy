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
#include "fpag/mem/arena.h"
#include "lexer/token.h"
#include "source/source.h"

namespace parser {

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
         mem::Arena& arena,
         diag::DiagBag& bag);

  // Parses a whole file into items. Always returns; errors accumulate
  // in the bag and erroneous constructs are simply absent.
  std::span<ast::Item* const> parse();

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
  ast::Item* parse_item();
  ast::FnItem* parse_fn(bool is_pub);
  ast::StructItem* parse_struct(bool is_pub);
  ast::EnumItem* parse_enum(bool is_pub);
  ast::ImplItem* parse_impl(bool is_pub);
  ast::StaticItem* parse_static(bool is_pub);
  ast::ConstItem* parse_const(bool is_pub);
  ast::UseItem* parse_use(bool is_pub);

  // Types. parse_closed_type additionally rejects a dangling banked
  // ">" left over from splitting ">>" in nested argument lists.
  ast::Type* parse_type();
  ast::Type* parse_closed_type();

  // Patterns and expressions (null on error, no recovery inside).
  ast::Pattern* parse_pattern();
  ast::Pattern* parse_or_pattern();
  ast::Pattern* parse_primary_pattern();
  ast::Expr* parse_expr();
  ast::Expr* parse_range();
  ast::Expr* parse_or();
  ast::Expr* parse_and();
  ast::Expr* parse_cmp();
  ast::Expr* parse_bitor();
  ast::Expr* parse_bitxor();
  ast::Expr* parse_bitand();
  ast::Expr* parse_shift();
  ast::Expr* parse_add();
  ast::Expr* parse_mul();
  ast::Expr* parse_pow();
  ast::Expr* parse_cast();
  ast::Expr* parse_unary();
  ast::Expr* parse_postfix();
  ast::Expr* parse_primary();
  ast::Expr* parse_if();
  ast::Expr* parse_match();
  ast::Expr* parse_loop();
  ast::Expr* parse_while();
  ast::Expr* parse_block_expr();
  ast::Block* parse_block();

  // Statements (recover at boundaries).
  ast::Stmt* parse_stmt();

  // Small pieces.
  base::Result<ast::Ident, diag::Fatal> parse_ident(std::string_view what);
  ast::Path* parse_path();
  ast::Cond* parse_cond();
  bool consume_gt();

  std::span<const lexer::Token> tokens_;
  std::string_view bytes_;
  source::FileId file_;
  mem::Arena& arena_;
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
