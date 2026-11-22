// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/parser.h"

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "ast/verify.h"
#include "base/nesting.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/attribute.h"
#include "parser/diag_code.h"
#include "source/source.h"

namespace parser {

namespace {

// The primitive type tokens, for the declaration a neighboring
// language spells with the type first.
bool is_primitive_type_token(lexer::TokenKind kind) {
  using K = lexer::TokenKind;
  switch (kind) {
    case K::I8:
    case K::I16:
    case K::I32:
    case K::I64:
    case K::I128:
    case K::Isize:
    case K::U8:
    case K::U16:
    case K::U32:
    case K::U64:
    case K::U128:
    case K::Usize:
    case K::F32:
    case K::F64:
    case K::Bool:
    case K::Str: return true;
    default: return false;
  }
}

bool is_compound_assign(lexer::TokenKind kind) {
  return (TOKEN_FLAGS[static_cast<u8>(kind)] & kTokenCompoundAssign) != 0;
}

ast::BinaryOp compound_op(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::PlusEq: return ast::BinaryOp::Add;
    case lexer::TokenKind::MinusEq: return ast::BinaryOp::Sub;
    case lexer::TokenKind::StarEq: return ast::BinaryOp::Mul;
    case lexer::TokenKind::SlashEq: return ast::BinaryOp::Div;
    case lexer::TokenKind::PercentEq: return ast::BinaryOp::Mod;
    case lexer::TokenKind::StarStarEq: return ast::BinaryOp::Pow;
    case lexer::TokenKind::AmpEq: return ast::BinaryOp::BitAnd;
    case lexer::TokenKind::PipeEq: return ast::BinaryOp::BitOr;
    case lexer::TokenKind::CaretEq: return ast::BinaryOp::BitXor;
    case lexer::TokenKind::LessLessEq: return ast::BinaryOp::Shl;
    default: return ast::BinaryOp::Shr;
  }
}

bool is_path_segment(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Ident:
    case lexer::TokenKind::Self:
    case lexer::TokenKind::Package:
    case lexer::TokenKind::SelfType: return true;
    default: return false;
  }
}

// A scalar spelling, which a path may open with: `i64::parse(text)`
// reaches a static member of the scalar.
bool is_primitive_path_head(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::I8:
    case lexer::TokenKind::I16:
    case lexer::TokenKind::I32:
    case lexer::TokenKind::I64:
    case lexer::TokenKind::Isize:
    case lexer::TokenKind::U8:
    case lexer::TokenKind::U16:
    case lexer::TokenKind::U32:
    case lexer::TokenKind::U64:
    case lexer::TokenKind::Usize:
    case lexer::TokenKind::F32:
    case lexer::TokenKind::F64: return true;
    default: return false;
  }
}

}  // namespace

Parser::Parser(std::span<const lexer::Token> tokens,
               std::string_view bytes,
               source::FileId file,
               ast::AstArena& ast,
               diag::DiagBag& bag,
               BuildFacts facts)
    : tokens_(tokens),
      bytes_(bytes),
      file_(file),
      facts_(facts),
      ast_(ast),
      bag_(bag),
      previous_span_({file, 0, 0}) {
  skip_insignificant();
}

lexer::TokenKind Parser::peek_kind(usize ahead) const {
  usize i = pos_;
  while (i < tokens_.size()) {
    const lexer::TokenKind kind = tokens_[i].kind;
    if (!is_insignificant(kind)) {
      if (ahead == 0) {
        return kind;
      }
      --ahead;
    }
    ++i;
  }
  return lexer::TokenKind::Eof;
}

void Parser::skip_insignificant() {
  while (pos_ < tokens_.size()) {
    const lexer::TokenKind kind = tokens_[pos_].kind;
    if (kind == lexer::TokenKind::Error) {
      ++pos_;
      continue;
    }
    if (kind == lexer::TokenKind::DocComment) {
      // A doc comment is trivia, and the item it stands above claims
      // it: the parser copies the spans onto the node it opens.
      pending_docs_.push_back(tokens_[pos_].span);
      ++pos_;
      continue;
    }
    if (is_reserved(kind)) {
      const diag::Span span = tokens_[pos_].span;
      const std::string_view spelling = bytes_.substr(span.offset, span.length);
      const u32 index = bag_.emit<i18n::Key::ParserReservedName>(
          DiagCode::ReservedWord, span, spelling);
      (void)index;
      ++pos_;
      continue;
    }
    break;
  }
}

Parser::Attributes Parser::parse_attribute_list() {
  Attributes attributes;
  while (peek_kind() == lexer::TokenKind::Hash) {
    const Attributes one = parse_attribute();
    attributes.merge_from(one);
    // A newline after `#[...]` lexes as `;`; it terminates the
    // attribute's line, not the item, so the item sits on its own.
    while (match(lexer::TokenKind::Semicolon)) {}
  }
  return attributes;
}

void Parser::synchronize_attribute() {
  i32 depth = 0;
  while (!at_end()) {
    const lexer::TokenKind kind = peek_kind();
    if (kind == lexer::TokenKind::LBracket ||
        kind == lexer::TokenKind::LParen) {
      ++depth;
      advance();
      continue;
    }
    if (kind == lexer::TokenKind::RBracket) {
      if (depth == 0) {
        advance();
        return;
      }
      --depth;
      advance();
      continue;
    }
    if (kind == lexer::TokenKind::RParen) {
      if (depth > 0) {
        --depth;
      }
      advance();
      continue;
    }
    if (kind == lexer::TokenKind::Semicolon && depth == 0) {
      return;
    }
    advance();
  }
}

Parser::Attributes Parser::parse_attribute() {
  Attributes nothing;
  // The cursor rests on `#`.
  const lexer::Token hash = peek();
  advance();
  if (!match(lexer::TokenKind::LBracket)) {
    const u32 index = bag_.emit<i18n::Key::ParserWhenMalformed>(
        DiagCode::WhenMalformed, hash.span);
    (void)index;
    synchronize_attribute();
    return nothing;
  }
  const lexer::Token name = peek();
  if (name.kind != lexer::TokenKind::Ident) {
    const u32 index = bag_.emit<i18n::Key::ParserWhenMalformed>(
        DiagCode::WhenMalformed, peek().span);
    (void)index;
    synchronize_attribute();
    return nothing;
  }
  const std::string_view attr_name =
      bytes_.substr(name.span.offset, name.span.length);
  advance();
  if (attr_name == "test") {
    // A role marker, never a gate: `#[test]` stands alone and the
    // existence question belongs to `#[when(testing)]`.
    if (check(lexer::TokenKind::LParen)) {
      const u32 index = bag_.emit<i18n::Key::ParserTestMalformed>(
          DiagCode::TestMalformed, name.span);
      (void)index;
      synchronize_attribute();
      return nothing;
    }
    if (!match(lexer::TokenKind::RBracket)) {
      const u32 index = bag_.emit<i18n::Key::ParserTestMalformed>(
          DiagCode::TestMalformed, name.span);
      (void)index;
      synchronize_attribute();
      return nothing;
    }
    Attributes result;
    result.is_test = true;
    result.test_span = name.span;
    return result;
  }
  if (attr_name == "repr") {
    return parse_repr_attribute(name.span);
  }
  if (attr_name != "when") {
    const u32 index = bag_.emit<i18n::Key::ParserUnknownAttribute>(
        DiagCode::UnknownAttribute, name.span, attr_name);
    (void)index;
    synchronize_attribute();
    return nothing;
  }
  if (!expect(lexer::TokenKind::LParen, "`(` after `when`")) {
    synchronize_attribute();
    return nothing;
  }
  // The inside of the argument list, from the token after `(` to the
  // matching `)`. Parentheses nest, so a plain index walk with a depth
  // count finds the end.
  const usize start = pos_;
  i32 depth = 0;
  while (!at_end()) {
    const lexer::TokenKind kind = peek_kind();
    if (kind == lexer::TokenKind::LParen) {
      ++depth;
      advance();
      continue;
    }
    if (kind == lexer::TokenKind::RParen) {
      if (depth == 0) {
        break;
      }
      --depth;
      advance();
      continue;
    }
    advance();
  }
  const usize end = pos_;
  (void)hash;
  if (!expect(lexer::TokenKind::RParen, "`)` ending `when(...)`")) {
    synchronize_attribute();
    return nothing;
  }
  if (!match(lexer::TokenKind::RBracket)) {
    const lexer::Token here = peek();
    const u32 index = bag_.emit<i18n::Key::ParserWhenMalformed>(
        DiagCode::WhenMalformed, here.span);
    (void)index;
    synchronize_attribute();
    return nothing;
  }
  const std::span<const lexer::Token> inside(tokens_.data() + start,
                                             end - start);
  const parser::GateOutcome outcome =
      evaluate_when(inside, bytes_, facts_, bag_);
  Attributes result;
  result.when_false = outcome == GateOutcome::Drop;
  return result;
}

Parser::Attributes Parser::parse_repr_attribute(diag::Span name_span) {
  Attributes out;
  if (!expect(lexer::TokenKind::LParen, "`(` after `repr`")) {
    synchronize_attribute();
    return out;
  }
  const lexer::Token arg = peek();
  if (arg.kind != lexer::TokenKind::Ident) {
    const u32 index = bag_.emit<i18n::Key::ParserReprMalformed>(
        DiagCode::ReprMalformed, arg.span);
    (void)index;
    synchronize_attribute();
    return out;
  }
  const std::string_view spelling =
      bytes_.substr(arg.span.offset, arg.span.length);
  if (spelling != "C") {
    const u32 index = bag_.emit<i18n::Key::ParserReprUnknown>(
        DiagCode::ReprUnknown, arg.span, spelling);
    (void)index;
    synchronize_attribute();
    return out;
  }
  advance();  // `C`
  if (!match(lexer::TokenKind::RParen)) {
    const u32 index = bag_.emit<i18n::Key::ParserReprMalformed>(
        DiagCode::ReprMalformed, peek().span);
    (void)index;
    synchronize_attribute();
    return out;
  }
  if (!match(lexer::TokenKind::RBracket)) {
    const u32 index = bag_.emit<i18n::Key::ParserReprMalformed>(
        DiagCode::ReprMalformed, peek().span);
    (void)index;
    synchronize_attribute();
    return out;
  }
  out.repr_c = true;
  out.repr_span = name_span;
  return out;
}

bool Parser::doubled_text(usize pos) const {
  if (pos + 1 >= tokens_.size()) {
    return false;
  }
  const diag::Span first = tokens_[pos].span;
  const diag::Span second = tokens_[pos + 1].span;
  return first.offset + first.length == second.offset;
}

void Parser::report_unexpected(std::string_view what) {
  if (at_end()) {
    const u32 index = bag_.emit<i18n::Key::ParserExpectedFoundEndOfFile>(
        DiagCode::UnexpectedToken, span_from(pos_), what);
    (void)index;
    return;
  }
  const diag::Span span = peek().span;
  const u32 index = bag_.emit<i18n::Key::ParserExpectedFound>(
      DiagCode::UnexpectedToken, span, what,
      bytes_.substr(span.offset, span.length));
  (void)index;
}

void Parser::synchronize() {
  const usize start = pos_;
  i32 depth = 0;
  while (!at_end()) {
    const lexer::TokenKind kind = peek_kind();
    if (depth == 0 &&
        (kind == lexer::TokenKind::Semicolon ||
         kind == lexer::TokenKind::Comma || kind == lexer::TokenKind::RBrace ||
         kind == lexer::TokenKind::RBracket ||
         kind == lexer::TokenKind::RParen)) {
      break;
    }
    if (kind == lexer::TokenKind::LBrace ||
        kind == lexer::TokenKind::LBracket ||
        kind == lexer::TokenKind::LParen) {
      ++depth;
    } else if ((kind == lexer::TokenKind::RBrace ||
                kind == lexer::TokenKind::RBracket ||
                kind == lexer::TokenKind::RParen) &&
               depth > 0) {
      --depth;
    }
    advance();
  }
  // Boundary tokens are left for the enclosing construct, but recovery
  // must always make progress: stealing one closer beats looping forever
  // on pathological input.
  if (pos_ == start && !at_end()) {
    advance();
  } else if (!at_end() && (peek_kind() == lexer::TokenKind::Semicolon ||
                           peek_kind() == lexer::TokenKind::Comma)) {
    advance();
  }
}

Parser::StmtLead Parser::scan_lead() const {
  i32 depth = 0;
  const usize count = tokens_.size();
  for (usize i = pos_; i < count;) {
    const u8 step = TOKEN_LEAD[static_cast<u8>(tokens_[i].kind)];
    ++i;
    if (step == kLeadOther) {
      continue;
    }
    if (step <= kLeadStmtEnd) {
      if (step == kLeadInsignificant) {
        continue;
      }
      if (depth == 0) {
        return step == kLeadDecl       ? StmtLead::Decl
               : step == kLeadReassign ? StmtLead::Reassign
                                       : StmtLead::None;
      }
      continue;
    }
    // A `{` is not a boundary by itself: a struct pattern in a
    // declaration or a condition carries one before the `:=`, so
    // braces count as nesting here like parens and brackets.
    if (step == kLeadRBrace) {
      if (depth == 0) {
        return StmtLead::None;
      }
      --depth;
      continue;
    }
    if (step == kLeadOpens) {
      ++depth;
    } else if (depth > 0) {
      --depth;
    }
  }
  return StmtLead::None;
}

bool Parser::scan_closure() const {
  using K = lexer::TokenKind;
  usize i = pos_;
  if (i < tokens_.size() && tokens_[i].kind == K::LBracket) {
    // A capture list: match the brackets, then a parameter list
    // must follow.
    i32 depth = 1;
    ++i;
    while (i < tokens_.size()) {
      const lexer::TokenKind kind = tokens_[i].kind;
      if (kind == K::Error || kind == K::DocComment || is_reserved(kind)) {
        ++i;
        continue;
      }
      if (kind == K::Semicolon || kind == K::RBrace || kind == K::Eof) {
        return false;
      }
      if (kind == K::LBracket || kind == K::LBrace || kind == K::LParen) {
        ++depth;
      } else if (kind == K::RBracket || kind == K::RBrace ||
                 kind == K::RParen) {
        --depth;
        if (depth == 0) {
          ++i;
          break;
        }
      }
      ++i;
    }
  }
  // The token cursor skips lexer errors, doc comments, and reserved
  // words, so the scan has to as well: a doc comment between the
  // capture list and the parameters is not a boundary.
  while (i < tokens_.size()) {
    const lexer::TokenKind kind = tokens_[i].kind;
    if (kind == K::Error || kind == K::DocComment || is_reserved(kind)) {
      ++i;
      continue;
    }
    break;
  }
  if (i >= tokens_.size() || tokens_[i].kind != K::LParen) {
    return false;
  }
  // The parameter list: a `:` at depth zero can only ascribe a
  // closure parameter, and an `->` after the matching `)` can
  // only open a closure body.
  i32 depth = 1;
  ++i;
  while (i < tokens_.size()) {
    const lexer::TokenKind kind = tokens_[i].kind;
    if (kind == K::Error || kind == K::DocComment || is_reserved(kind)) {
      ++i;
      continue;
    }
    if (kind == K::Semicolon || kind == K::RBrace || kind == K::Eof) {
      return false;
    }
    if (depth == 0) {
      return kind == K::Arrow;
    }
    // A `:` at the group's own depth can only ascribe a closure
    // parameter: struct fields nest a level deeper, and `::` is
    // its own token.
    if (kind == K::Colon && depth == 1) {
      return true;
    }
    if (kind == K::LBracket || kind == K::LBrace || kind == K::LParen) {
      ++depth;
    } else if (kind == K::RBracket || kind == K::RBrace || kind == K::RParen) {
      --depth;
    }
    ++i;
  }
  return false;
}

base::Result<std::span<const ast::ItemIdx>, diag::Reported> Parser::parse() {
  if (base::Result<void, lexer::TokenStreamError> verified =
          lexer::verify_token_stream(tokens_, file_, bytes_);
      verified.is_err()) {
    const u32 index = bag_.emit<i18n::Key::ParserInvalidTokenStream>(
        DiagCode::InvalidTokenStream,
        lexer::describe_token_stream_error(std::move(verified).unwrap_err()));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  std::vector<ast::ItemIdx> items;
  parse_items_until(std::nullopt, Attributes{}, items);
  return base::make_ok(
      std::span<const ast::ItemIdx>(ast::copy_to_arena(ast_.spans, items)));
}

base::Result<void, diag::Reported> verify_trees(ast::AstArena& ast,
                                                diag::DiagBag& bag) {
  if (base::Result<void, ast::VerificationError> verified =
          ast::verify_file(ast);
      verified.is_err()) {
    const u32 index = bag.emit<i18n::Key::ParserInvalidSyntaxTree>(
        DiagCode::InvalidAst,
        ast::describe_verification_error(std::move(verified).unwrap_err()));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  ast.mark_verified();
  return base::make_ok();
}

std::optional<ast::ItemIdx> Parser::parse_spelled_item(bool is_pub,
                                                       bool is_unsafe,
                                                       bool is_comp) {
  using K = lexer::TokenKind;
  const lexer::Token start = peek();
  if (start.kind != K::Ident) {
    return std::nullopt;
  }
  const std::string_view word =
      bytes_.substr(start.span.offset, start.span.length);
  if ((word == "func" || word == "def" || word == "function") &&
      peek_kind(1) == K::Ident) {
    const u32 index = bag_.emit<i18n::Key::ParserFnItemSpelling>(
        DiagCode::Spelling, start.span);
    (void)index;
    advance();
    return parse_fn(is_pub, is_unsafe, is_comp, /*keyword_consumed=*/true);
  }
  if (word == "trait" && peek_kind(1) == K::Ident) {
    const u32 index = bag_.emit<i18n::Key::ParserSpecItemSpelling>(
        DiagCode::Spelling, start.span);
    (void)index;
    advance();
    return parse_spec(is_pub, /*keyword_consumed=*/true);
  }
  if (word == "import" && peek_kind(1) == K::Ident) {
    const u32 index = bag_.emit<i18n::Key::ParserUseItemSpelling>(
        DiagCode::Spelling, start.span);
    (void)index;
    advance();
    return parse_use(is_pub, /*keyword_consumed=*/true);
  }
  if (word == "mod") {
    const u32 index = bag_.emit<i18n::Key::ParserModuleFromManifest>(
        DiagCode::Spelling, start.span);
    (void)index;
    while (!at_end() && !check(K::Semicolon) && !check(K::RBrace)) {
      advance();
    }
    match(K::Semicolon);
    return ast::ItemIdx::invalid();
  }
  return std::nullopt;
}

void Parser::parse_items_until(std::optional<lexer::TokenKind> terminator,
                               Attributes inherited,
                               std::vector<ast::ItemIdx>& out) {
  using K = lexer::TokenKind;
  while (!at_end()) {
    if (terminator.has_value() && check(*terminator)) {
      break;
    }
    if (match(K::Semicolon)) {
      continue;
    }
    Attributes attributes = inherited;
    bool has_attributes = false;
    if (peek_kind() == K::Hash) {
      const Attributes own = parse_attribute_list();
      has_attributes = true;
      attributes.merge_from(own);
    }
    // A brace opens a group only behind an attribute. A bare brace is
    // not an item, and reads as a parse error the way tree-sitter does.
    if (has_attributes && peek_kind() == K::LBrace) {
      parse_conditional_group(attributes, out);
      continue;
    }
    ast::ItemIdx item = parse_documented_item(attributes);
    if (!item.is_valid()) {
      if (ast_.exhausted()) {
        break;
      }
      synchronize();
      continue;
    }
    // An item the gate dropped exists nowhere. The module loop sees
    // gates in `attributes`; a group passes its own gate through
    // `inherited`, so both paths land here.
    if (ast_.items[item].when_false) {
      continue;
    }
    out.push_back(item);
  }
}

void Parser::parse_conditional_group(Attributes attributes,
                                     std::vector<ast::ItemIdx>& out) {
  using K = lexer::TokenKind;
  if (ast_.exhausted()) {
    return;
  }
  if (nesting_exhausted(peek().span)) {
    return;
  }
  const base::NestingScope scope(nesting_);
  // A group has no AST node of its own. Doc comments written above it
  // belong to no item, so drop them rather than let the first child
  // claim them.
  pending_docs_.clear();
  // A group is a gate, not an item. A role or layout marker written on
  // the brace would otherwise reach every function or struct inside it.
  // Refuse it here and keep it from the children.
  if (attributes.is_test) {
    const u32 index = bag_.emit<i18n::Key::ParserTestOnlyOnFunctions>(
        DiagCode::UnexpectedToken, attributes.test_span);
    (void)index;
    attributes.is_test = false;
  }
  if (attributes.repr_c) {
    const u32 index = bag_.emit<i18n::Key::ParserReprNotOnStruct>(
        DiagCode::ReprNotOnStruct, attributes.repr_span);
    (void)index;
    attributes.repr_c = false;
  }
  if (!expect(K::LBrace, "`{`")) {
    return;
  }
  parse_items_until(K::RBrace, attributes, out);
  if (!expect(K::RBrace, "`}`")) {
    return;
  }
}

ast::ItemIdx Parser::parse_documented_item(Attributes attributes) {
  // The doc comments above the item are the item's own; the ones the
  // parse skips inside it wait for the next item.
  skip_insignificant();
  const usize docs = pending_docs_.size();
  ast::ItemIdx item = parse_item(attributes);
  if (!item.is_valid()) {
    pending_docs_.erase(pending_docs_.begin(), pending_docs_.begin() + docs);
    return item;
  }
  const std::span<const diag::Span> spans = ast::copy_to_arena(
      ast_.spans, std::span<const diag::Span>(pending_docs_.data(), docs));
  pending_docs_.erase(pending_docs_.begin(), pending_docs_.begin() + docs);
  ast_.items[item].docs = spans;
  return item;
}

bool Parser::apply_test_marker(ast::ItemIdx item, diag::Span span) {
  ast::ItemNode& node = ast_.items[item];
  if (node.kind != ast::ItemKind::Fn) {
    const u32 index = bag_.emit<i18n::Key::ParserTestOnlyOnFunctions>(
        DiagCode::UnexpectedToken, span);
    (void)index;
    return false;
  }
  const ast::ItemFn& fn = node.payload.get<ast::ItemFn>();
  if (fn.params.size() != 0) {
    const u32 index = bag_.emit<i18n::Key::ParserTestTakesNoArguments>(
        DiagCode::UnexpectedToken, span);
    (void)index;
    return false;
  }
  if (!fn.generic.empty()) {
    const u32 index = bag_.emit<i18n::Key::ParserTestHasTypeParameters>(
        DiagCode::UnexpectedToken, span);
    (void)index;
    return false;
  }
  // The driver calls a test with a plain call at runtime, so neither
  // opt-in fits one. Refuse here rather than reporting against code the
  // run generated for the caller.
  if (fn.is_unsafe) {
    const u32 index = bag_.emit<i18n::Key::ParserTestOnUnsafe>(
        DiagCode::UnexpectedToken, span);
    (void)index;
    return false;
  }
  if (fn.is_comp) {
    const u32 index =
        bag_.emit<i18n::Key::ParserTestOnComp>(DiagCode::UnexpectedToken, span);
    (void)index;
    return false;
  }
  ast::ItemFn marked = fn;
  marked.is_test = true;
  node.payload.set(marked);
  return true;
}

ast::ItemIdx Parser::parse_item(Attributes attributes) {
  // A spent arena builds nothing further: what it refused would leave a
  // node half made and a diagnostic about the wrong thing.
  if (ast_.exhausted()) {
    return ast::ItemIdx::invalid();
  }
  const auto finish = [&](ast::ItemIdx item) {
    if (item.is_valid()) {
      ast::ItemNode& node = ast_.items[item];
      node.when_false = attributes.when_false;
      if (attributes.is_test && !apply_test_marker(item, node.span)) {
        return ast::ItemIdx::invalid();
      }
      if (attributes.repr_c) {
        if (node.kind != ast::ItemKind::Struct) {
          const u32 index = bag_.emit<i18n::Key::ParserReprNotOnStruct>(
              DiagCode::ReprNotOnStruct, attributes.repr_span);
          (void)index;
        } else if (node.payload.get<ast::ItemStruct>().fields.empty()) {
          const u32 index = bag_.emit<i18n::Key::ParserReprEmpty>(
              DiagCode::ReprEmpty, attributes.repr_span);
          (void)index;
        } else {
          node.repr_c = true;
        }
      }
    }
    return item;
  };
  const bool is_pub = match(lexer::TokenKind::Pub);
  const bool is_unsafe = match(lexer::TokenKind::Unsafe);
  // `comp` in item position marks a free function ; a
  // receiver method keeps the marker for a later slice. The two
  // markers do not combine yet, in either spelling order.
  const lexer::Token comp_token = peek();
  const bool is_comp = match(lexer::TokenKind::Comp);
  const auto refuse_comp_unsafe = [&] {
    const u32 index = bag_.emit<i18n::Key::ParserCompUnsafeCombination>(
        DiagCode::UnexpectedToken, comp_token.span);
    (void)index;
    return finish(ast::ItemIdx::invalid());
  };
  if (is_unsafe && is_comp) {
    return finish(refuse_comp_unsafe());
  }
  if (is_comp && peek_kind() == lexer::TokenKind::Unsafe) {
    advance();
    return finish(refuse_comp_unsafe());
  }
  // An item keyword from a neighboring language is parsed as the item
  // it meant.
  if (const std::optional<ast::ItemIdx> spelled =
          parse_spelled_item(is_pub, is_unsafe, is_comp)) {
    return finish(*spelled);
  }
  if (is_comp && peek_kind() != lexer::TokenKind::Fn) {
    const u32 index = bag_.emit<i18n::Key::ParserCompOnlyOnFunctions>(
        DiagCode::UnexpectedToken, comp_token.span);
    (void)index;
    return finish(ast::ItemIdx::invalid());
  }
  // `unsafe` marks an operation, and only functions declare one, so
  // it is refused anywhere else rather than read past.
  const auto only_functions = [&] {
    if (!is_unsafe) {
      return true;
    }
    const u32 index = bag_.emit<i18n::Key::ParserUnsafeOnlyOnFunctions>(
        DiagCode::UnexpectedToken, peek().span);
    (void)index;
    return false;
  };
  switch (peek_kind()) {
    case lexer::TokenKind::Fn:
      return finish(parse_fn(is_pub, is_unsafe, is_comp));
    case lexer::TokenKind::Intrinsic:
      return finish(parse_intrinsic_fn(is_pub, is_unsafe));
    case lexer::TokenKind::Extern: {
      only_functions();
      return finish(parse_extern(is_pub));
    }
    case lexer::TokenKind::Struct:
      only_functions();
      return finish(parse_struct(is_pub));
    case lexer::TokenKind::Enum:
      only_functions();
      return finish(parse_enum(is_pub));
    case lexer::TokenKind::Impl:
      only_functions();
      return finish(parse_impl(is_pub));
    case lexer::TokenKind::Spec:
      only_functions();
      return finish(parse_spec(is_pub));
    case lexer::TokenKind::Static:
      only_functions();
      return finish(parse_static(is_pub));
    case lexer::TokenKind::Const:
      only_functions();
      return finish(parse_const(is_pub));
    case lexer::TokenKind::Use:
      only_functions();
      return finish(parse_use(is_pub));
    default: break;
  }
  if (at_end()) {
    const u32 index = bag_.emit<i18n::Key::ParserExpectedItemFoundEndOfFile>(
        DiagCode::UnexpectedToken, span_from(pos_));
    (void)index;
    return finish(ast::ItemIdx::invalid());
  }
  const diag::Span span = peek().span;
  const u32 index = bag_.emit<i18n::Key::ParserExpectedItemFound>(
      DiagCode::UnexpectedToken, span, bytes_.substr(span.offset, span.length));
  (void)index;
  return ast::ItemIdx::invalid();
}

base::Result<ast::Ident, diag::Reported> Parser::parse_ident(
    std::string_view what) {
  if (peek_kind() != lexer::TokenKind::Ident) {
    expect(lexer::TokenKind::Ident, what);
    return base::make_err(diag::Reported{});
  }
  const diag::Span span = peek().span;
  ast::Ident id{bytes_.substr(span.offset, span.length), span};
  advance();
  return base::make_ok(id);
}

ast::PathIdx Parser::parse_path(std::vector<ast::TypeIdx>* type_args,
                                u32* type_args_segment) {
  const usize mark = pos_;
  // Nine paths in ten are one segment with nothing after it, and the walk
  // below spends a step of the frame, the spill test, and the loop on the
  // one. That shape is answered here, and everything else goes the long
  // way.
  if (is_path_segment(peek_kind()) &&
      (pos_ + 1 >= tokens_.size() ||
       tokens_[pos_ + 1].kind != lexer::TokenKind::ColonColon)) {
    const diag::Span span = peek().span;
    const ast::Ident only{bytes_.substr(span.offset, span.length), span};
    advance();
    ast::Path one;
    one.segments =
        ast::copy_to_arena(ast_.spans, std::span<const ast::Ident>(&only, 1));
    one.span = span_from(mark);
    return ast_.paths.push_back(one);
  }
  // A longer path. The segments live on the frame because a turbofish
  // argument list may parse another path before this one is copied, so the
  // storage cannot be the parser's; a path that outgrows the frame spills
  // once.
  constexpr usize INLINE_PATH_SEGMENTS = 4;
  ast::Ident inline_segments[INLINE_PATH_SEGMENTS];
  std::vector<ast::Ident> spilled_segments;
  usize segment_count = 0;
  const auto push_segment = [&](const ast::Ident segment) {
    if (segment_count >= INLINE_PATH_SEGMENTS && spilled_segments.empty()) {
      spilled_segments.assign(inline_segments,
                              inline_segments + INLINE_PATH_SEGMENTS);
    }
    if (spilled_segments.empty()) {
      inline_segments[segment_count] = segment;
    } else {
      spilled_segments.push_back(segment);
    }
    ++segment_count;
  };
  while (true) {
    // A scalar may open a path only as a member's prefix: `i64::parse`
    // parses, a bare `i64` expression stays the spelling error it was.
    const lexer::TokenKind kind = peek_kind();
    const bool scalar_head =
        segment_count == 0 && is_primitive_path_head(kind) &&
        pos_ + 1 < tokens_.size() &&
        tokens_[pos_ + 1].kind == lexer::TokenKind::ColonColon;
    if (!is_path_segment(kind) && !scalar_head) {
      if (segment_count == 0) {
        expect(lexer::TokenKind::Ident, "path");
        return ast::PathIdx::invalid();
      }
      break;
    }
    const diag::Span span = peek().span;
    push_segment(ast::Ident{bytes_.substr(span.offset, span.length), span});
    advance();
    if (peek_kind() != lexer::TokenKind::ColonColon) {
      break;
    }
    // `::<` is a turbofish type-argument list rather than another
    // segment. Only an expression path can carry one, so a type path
    // stops here and its caller reads the arguments.
    if (pos_ + 1 >= tokens_.size() ||
        tokens_[pos_ + 1].kind != lexer::TokenKind::Less) {
      advance();
      if (at_end()) {
        expect(lexer::TokenKind::Ident, "path segment");
        return ast::PathIdx::invalid();
      }
      continue;
    }
    if (type_args == nullptr || !type_args->empty()) {
      break;
    }
    advance();
    if (type_args_segment != nullptr) {
      *type_args_segment = static_cast<u32>(segment_count - 1);
    }
    if (!match(lexer::TokenKind::Less)) {
      expect(lexer::TokenKind::Less, "`<`");
      return ast::PathIdx::invalid();
    }
    if (!parse_turbofish(*type_args)) {
      return ast::PathIdx::invalid();
    }
    // `Name::<T>::member` continues with the member segment.
    if (peek_kind() != lexer::TokenKind::ColonColon) {
      break;
    }
    advance();
    if (at_end()) {
      expect(lexer::TokenKind::Ident, "path segment");
      return ast::PathIdx::invalid();
    }
  }
  const std::span<const ast::Ident> segments =
      spilled_segments.empty()
          ? std::span<const ast::Ident>(inline_segments, segment_count)
          : std::span<const ast::Ident>(spilled_segments);
  ast::Path path;
  path.segments = ast::copy_to_arena(ast_.spans, segments);
  path.span = span_from(mark);
  return ast_.paths.push_back(path);
}

bool Parser::parse_turbofish(std::vector<ast::TypeIdx>& type_args) {
  while (!check(lexer::TokenKind::Greater) &&
         !check(lexer::TokenKind::GreaterGreater) && !at_end()) {
    ast::TypeIdx arg = parse_type_arg();
    if (!arg.is_valid()) {
      return false;
    }
    type_args.push_back(arg);
    // A nested generic argument absorbs the `>` that closes the list as
    // part of its own `>>`; `consume_gt` drains the banked one.
    if (banked_gt_ > 0) {
      break;
    }
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!consume_gt()) {
    expect(lexer::TokenKind::Greater, "`>`");
    return false;
  }
  return true;
}

bool Parser::parse_fn_signature(FnSignature& signature, bool keyword_consumed) {
  if (!keyword_consumed && !expect(lexer::TokenKind::Fn, "function")) {
    return false;
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("function name");
  if (name.is_err()) {
    return false;
  }
  signature.name = std::move(name).unwrap();
  if (!parse_generic_params(signature.generic)) {
    return false;
  }
  if (!expect(lexer::TokenKind::LParen, "`(`")) {
    return false;
  }
  if (!parse_fn_params(signature.params)) {
    return false;
  }
  if (!expect(lexer::TokenKind::RParen, "`)`")) {
    return false;
  }
  if (match(lexer::TokenKind::Arrow)) {
    signature.return_type = parse_closed_type();
    if (!signature.return_type.is_valid()) {
      return false;
    }
  }
  return true;
}

ast::ItemIdx Parser::parse_fn(bool is_pub,
                              bool is_unsafe,
                              bool is_comp,
                              bool keyword_consumed) {
  const usize mark = pos_;
  FnSignature signature;
  if (!parse_fn_signature(signature, keyword_consumed)) {
    return ast::ItemIdx::invalid();
  }
  ast::BlockIdx body = parse_header_block();
  if (!body.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Fn;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemFn{
      .name = signature.name,
      .generic = ast::copy_to_arena(ast_.spans, signature.generic),
      .params = ast::copy_to_arena(ast_.spans, signature.params),
      .return_type = signature.return_type,
      .body = body,
      .is_unsafe = is_unsafe,
      .is_comp = is_comp,
  });
  return ast_.items.push_back(node);
}

bool Parser::parse_fn_params(std::vector<ast::ItemFnParam>& params) {
  if (!check(lexer::TokenKind::RParen)) {
    while (true) {
      const bool is_comp = match(lexer::TokenKind::Comp);
      ast::PatternIdx pattern = ast::PatternIdx::invalid();
      ast::TypeIdx type = ast::TypeIdx::invalid();
      bool is_variadic = false;
      if (ref_self_parameter_shape()) {
        const lexer::Token amp = peek();
        const u32 index = bag_.emit<i18n::Key::ParserRefSelfParameter>(
            DiagCode::Spelling, amp.span);
        (void)index;
        advance();  // `&`
        const bool is_mut = peek_kind() == lexer::TokenKind::Mut;
        pattern = parse_pattern();  // `mut self` or `self`
        if (!pattern.is_valid()) {
          return false;
        }
        type = ref_self_type(is_mut, amp.span);
      } else {
        pattern = parse_pattern();
        if (!pattern.is_valid()) {
          return false;
        }
        if (!expect(lexer::TokenKind::Colon, "`:`")) {
          return false;
        }
        if (check(lexer::TokenKind::DotDot)) {
          // `args: ..` collects the call's remaining arguments; the
          // name binds them and only the compiler expands the call.
          if (ast_.patterns[pattern].kind != ast::PatternKind::Ident) {
            const u32 index = bag_.emit<i18n::Key::ParserVariadicParameter>(
                DiagCode::UnexpectedToken, ast_.patterns[pattern].span);
            (void)index;
            return false;
          }
          advance();
          is_variadic = true;
        } else {
          type = parse_closed_type();
          if (!type.is_valid()) {
            return false;
          }
        }
      }
      params.push_back(ast::ItemFnParam{pattern, type, is_comp, is_variadic});
      if (is_variadic) {
        if (check(lexer::TokenKind::Comma)) {
          const u32 index = bag_.emit<i18n::Key::ParserVariadicParameter>(
              DiagCode::UnexpectedToken, ast_.patterns[pattern].span);
          (void)index;
          return false;
        }
        break;
      }
      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
      if (check(lexer::TokenKind::RParen)) {
        break;
      }
    }
  }
  return true;
}

bool Parser::ref_self_parameter_shape() const {
  using K = lexer::TokenKind;
  if (peek_kind() != K::Amp) {
    return false;
  }
  const K next = peek_kind(1);
  if (next == K::Self) {
    const K after = peek_kind(2);
    return after == K::Comma || after == K::RParen;
  }
  if (next == K::Mut && peek_kind(2) == K::Self) {
    const K after = peek_kind(3);
    return after == K::Comma || after == K::RParen;
  }
  return false;
}

ast::TypeIdx Parser::ref_self_type(bool is_mut, diag::Span span) {
  std::vector<ast::Ident> segments;
  segments.push_back(ast::Ident{"Self", span});
  ast::Path path;
  path.segments = ast::copy_to_arena(ast_.spans, segments);
  path.span = span;
  ast::TypeNode path_type;
  path_type.kind = ast::TypeKind::Path;
  path_type.span = span;
  path_type.payload.set(ast::TypePath{
      .path = ast_.paths.push_back(path),
      .args = {},
  });
  const ast::TypeIdx self_type = ast_.types.push_back(path_type);
  ast::TypeNode ref_type;
  ref_type.kind = ast::TypeKind::Ref;
  ref_type.span = span;
  ref_type.payload.set(ast::TypeRef{
      .is_mut = is_mut,
      .inner = self_type,
  });
  return ast_.types.push_back(ref_type);
}

ast::ItemIdx Parser::parse_intrinsic_fn(bool is_pub, bool is_unsafe) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Intrinsic, "`intrinsic`")) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Fn, "function")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("function name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::GenericParam> generic;
  if (!parse_generic_params(generic)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LParen, "`(`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemFnParam> params;
  if (!parse_fn_params(params)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::RParen, "`)`")) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx return_type = ast::TypeIdx::invalid();
  if (match(lexer::TokenKind::Arrow)) {
    return_type = parse_closed_type();
    if (!return_type.is_valid()) {
      return ast::ItemIdx::invalid();
    }
  }
  // Intrinsic declarations carry no body: the signature ends here.
  if (!expect(lexer::TokenKind::Semicolon, "`;`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Intrinsic;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemIntrinsic{
      .name = std::move(name).unwrap(),
      .generic = ast::copy_to_arena(ast_.spans, generic),
      .params = ast::copy_to_arena(ast_.spans, params),
      .return_type = return_type,
      .is_unsafe = is_unsafe,
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_extern(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Extern, "`extern`")) {
    return ast::ItemIdx::invalid();
  }
  if (!check(lexer::TokenKind::String)) {
    const u32 index = bag_.emit<i18n::Key::ParserExternNeedsConvention>(
        DiagCode::UnexpectedToken, peek().span);
    (void)index;
    return ast::ItemIdx::invalid();
  }
  const diag::Span convention_span = peek().span;
  const std::string_view raw =
      bytes_.substr(convention_span.offset, convention_span.length);
  advance();
  if (raw != "\"C\"") {
    const u32 index = bag_.emit<i18n::Key::ParserExternConvention>(
        DiagCode::UnexpectedToken, convention_span);
    (void)index;
    return ast::ItemIdx::invalid();
  }
  if (!expect_header_lbrace()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemExternFn> fns;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    FnSignature signature;
    if (!parse_fn_signature(signature)) {
      synchronize();
      continue;
    }
    // A declaration carries no body: the signature ends here.
    if (!expect(lexer::TokenKind::Semicolon, "`;`")) {
      synchronize();
      continue;
    }
    fns.push_back(ast::ItemExternFn{
        .name = signature.name,
        .generic = ast::copy_to_arena(ast_.spans, signature.generic),
        .params = ast::copy_to_arena(ast_.spans, signature.params),
        .return_type = signature.return_type,
    });
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode block;
  block.kind = ast::ItemKind::Extern;
  block.span = span_from(mark);
  block.is_pub = is_pub;
  block.payload.set(ast::ItemExtern{
      .convention = raw.substr(1, raw.size() - 2),
      .fns = ast::copy_to_arena(ast_.spans, fns),
  });
  return ast_.items.push_back(block);
}

ast::ItemIdx Parser::parse_struct(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Struct, "struct")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("struct name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::GenericParam> params;
  if (!parse_generic_params(params)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect_header_lbrace()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemStructField> fields;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    const bool field_pub = match(lexer::TokenKind::Pub);
    base::Result<ast::Ident, diag::Reported> field_name =
        parse_ident("field name");
    if (field_name.is_err()) {
      return ast::ItemIdx::invalid();
    }
    if (!expect(lexer::TokenKind::Colon, "`:`")) {
      return ast::ItemIdx::invalid();
    }
    ast::TypeIdx type = parse_closed_type();
    if (!type.is_valid()) {
      return ast::ItemIdx::invalid();
    }
    fields.push_back(
        ast::ItemStructField{std::move(field_name).unwrap(), type, field_pub});
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Struct;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemStruct{
      .name = std::move(name).unwrap(),
      .params = ast::copy_to_arena(ast_.spans, params),
      .fields = ast::copy_to_arena(ast_.spans, fields),
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_enum(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Enum, "enum")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("enum name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::GenericParam> params;
  if (!parse_generic_params(params)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect_header_lbrace()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemEnumVariant> variants;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    base::Result<ast::Ident, diag::Reported> variant_name =
        parse_ident("variant name");
    if (variant_name.is_err()) {
      return ast::ItemIdx::invalid();
    }
    std::vector<ast::TypeIdx> fields;
    if (match(lexer::TokenKind::LParen)) {
      if (!check(lexer::TokenKind::RParen)) {
        while (true) {
          ast::TypeIdx field = parse_type();
          if (!field.is_valid()) {
            return ast::ItemIdx::invalid();
          }
          fields.push_back(field);
          if (!match(lexer::TokenKind::Comma)) {
            break;
          }
          if (check(lexer::TokenKind::RParen)) {
            break;
          }
        }
      }
      if (!expect(lexer::TokenKind::RParen, "`)`")) {
        return ast::ItemIdx::invalid();
      }
    }
    variants.push_back(
        ast::ItemEnumVariant{std::move(variant_name).unwrap(),
                             ast::copy_to_arena(ast_.spans, fields)});
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Enum;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemEnum{
      .name = std::move(name).unwrap(),
      .params = ast::copy_to_arena(ast_.spans, params),
      .variants = ast::copy_to_arena(ast_.spans, variants),
  });
  return ast_.items.push_back(node);
}

bool Parser::parse_generic_params(std::vector<ast::GenericParam>& params) {
  if (!match(lexer::TokenKind::Less)) {
    return true;
  }
  bool closed = false;
  while (!check(lexer::TokenKind::Greater) && !at_end()) {
    base::Result<ast::Ident, diag::Reported> param =
        parse_ident("type parameter");
    if (param.is_err()) {
      return false;
    }
    ast::Ident name = std::move(param).unwrap();
    for (const ast::GenericParam& existing : params) {
      if (existing.name.name == name.name) {
        const u32 index = bag_.emit<i18n::Key::ParserDuplicateTypeParameter>(
            DiagCode::UnexpectedToken, name.span, name.name);
        (void)index;
        return false;
      }
    }
    ast::TypeIdx type = ast::TypeIdx::invalid();
    std::vector<ast::TypeIdx> extra_bounds;
    if (match(lexer::TokenKind::Colon)) {
      type = parse_type();
      if (!type.is_valid()) {
        return false;
      }
      // `T: A + B`: the annotations after the first are bounds too.
      while (match(lexer::TokenKind::Plus)) {
        const ast::TypeIdx bound = parse_type();
        if (!bound.is_valid()) {
          return false;
        }
        extra_bounds.push_back(bound);
      }
    }
    params.push_back(ast::GenericParam{
        name, type, ast::copy_to_arena(ast_.spans, extra_bounds)});
    // A nested generic annotation absorbs the `>` that closes the list
    // as part of its own `>>`; the banked half is that closer.
    if (banked_gt_ > 0) {
      consume_gt();
      closed = true;
      break;
    }
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!closed && !expect(lexer::TokenKind::Greater, "`>`")) {
    return false;
  }
  return true;
}

ast::ItemIdx Parser::parse_impl(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Impl, "impl")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::GenericParam> params;
  if (!parse_generic_params(params)) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx type = parse_closed_type();
  if (!type.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx spec = ast::TypeIdx::invalid();
  if (match(lexer::TokenKind::For)) {
    // `impl S for T`: the type just parsed is the spec side, so it
    // must be a plain path. Spec arguments (`Iterator<T>`) ride the
    // same type grammar as the target.
    if (ast_.types[type].kind != ast::TypeKind::Path) {
      const diag::Span span = ast_.types[type].span;
      const u32 index = bag_.emit<i18n::Key::ParserExpectedFound>(
          DiagCode::UnexpectedToken, span, "a spec",
          bytes_.substr(span.offset, span.length));
      (void)index;
      return ast::ItemIdx::invalid();
    }
    spec = type;
    type = parse_closed_type();
    if (!type.is_valid()) {
      return ast::ItemIdx::invalid();
    }
  }
  if (!expect_header_lbrace()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemIdx> methods;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    // `#[when(...)]` gates a method the same way it gates a free item:
    // a gate that evaluates to false on this target removes the method
    // from the block before anything downstream sees it.
    Attributes attributes;
    if (peek_kind() == lexer::TokenKind::Hash) {
      const diag::Span attribute_span = peek().span;
      attributes = parse_attribute_list();
      if (attributes.repr_c) {
        const u32 index = bag_.emit<i18n::Key::ParserReprNotOnStruct>(
            DiagCode::ReprNotOnStruct, attributes.repr_span);
        (void)index;
      }
      // A run gathers tests from module items, so a method cannot be one.
      // Without this the marker would be read and then dropped in silence.
      if (attributes.is_test) {
        const u32 index = bag_.emit<i18n::Key::ParserTestNotOnMethod>(
            DiagCode::UnexpectedToken, attribute_span);
        (void)index;
      }
    }
    // The two `match`es read the one token stream in order, so they are
    // statements and not arguments: a build that evaluates arguments
    // right to left reads `unsafe` while it is still at `pub`, and the
    // method arrives at `parse_fn` a token early.
    const bool is_pub = match(lexer::TokenKind::Pub);
    const bool is_unsafe = match(lexer::TokenKind::Unsafe);
    ast::ItemIdx method = parse_fn(is_pub, is_unsafe, false);
    if (!method.is_valid()) {
      synchronize();
      continue;
    }
    if (!attributes.when_false) {
      methods.push_back(method);
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Impl;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemImpl{
      .params = ast::copy_to_arena(ast_.spans, params),
      .type = type,
      .spec = spec,
      .methods = ast::copy_to_arena(ast_.spans, methods),
  });
  return ast_.items.push_back(node);
}

bool Parser::parse_spec_method(ast::SpecMethod& method) {
  const bool is_unsafe = match(lexer::TokenKind::Unsafe);
  FnSignature signature;
  if (!parse_fn_signature(signature)) {
    return false;
  }
  // No default bodies in v1: a spec declares signatures, and every
  // implementation supplies each one.
  if (!expect(lexer::TokenKind::Semicolon, "`;`")) {
    return false;
  }
  method = ast::SpecMethod{
      .name = signature.name,
      .generic = ast::copy_to_arena(ast_.spans, signature.generic),
      .params = ast::copy_to_arena(ast_.spans, signature.params),
      .return_type = signature.return_type,
      .is_unsafe = is_unsafe,
  };
  return true;
}

ast::ItemIdx Parser::parse_spec(bool is_pub, bool keyword_consumed) {
  const usize mark = pos_;
  if (!keyword_consumed && !expect(lexer::TokenKind::Spec, "spec")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("spec name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::GenericParam> params;
  if (!parse_generic_params(params)) {
    return ast::ItemIdx::invalid();
  }
  // `spec Eq: PartialEq` refines a super-spec: a path the
  // checker resolves to another spec, whose implementation every
  // implementation of this one then requires.
  ast::TypeIdx super = ast::TypeIdx::invalid();
  if (match(lexer::TokenKind::Colon)) {
    super = parse_closed_type();
    if (!super.is_valid()) {
      return ast::ItemIdx::invalid();
    }
  }
  if (!expect_header_lbrace()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::SpecMethod> methods;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    if (match(lexer::TokenKind::Pub)) {
      // Method visibility is not checked; accept the marker for
      // consistency with inherent impl blocks.
    }
    ast::SpecMethod method;
    if (!parse_spec_method(method)) {
      synchronize();
      continue;
    }
    methods.push_back(method);
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Spec;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemSpec{
      .name = std::move(name).unwrap(),
      .params = ast::copy_to_arena(ast_.spans, params),
      .super = super,
      .methods = ast::copy_to_arena(ast_.spans, methods),
  });
  return ast_.items.push_back(node);
}

bool Parser::expect_static_const_equals() {
  if (peek_kind() == lexer::TokenKind::Eq) {
    const lexer::Token eq = peek();
    (void)bag_.emit<i18n::Key::ParserStaticConstSpelling>(DiagCode::Spelling,
                                                          eq.span);
    match(lexer::TokenKind::Eq);
    return true;
  }
  return expect(lexer::TokenKind::ColonEq, "`:=`");
}

ast::ItemIdx Parser::parse_static(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Static, "static")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("static name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Colon, "`:`")) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx type = parse_closed_type();
  if (!type.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect_static_const_equals()) {
    return ast::ItemIdx::invalid();
  }
  ast::ExprIdx init = parse_expr();
  if (!init.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Static;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemStatic{
      .name = std::move(name).unwrap(),
      .type = type,
      .init = init,
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_const(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Const, "const")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("const name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Colon, "`:`")) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx type = parse_closed_type();
  if (!type.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect_static_const_equals()) {
    return ast::ItemIdx::invalid();
  }
  ast::ExprIdx init = parse_expr();
  if (!init.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Const;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemConst{
      .name = std::move(name).unwrap(),
      .type = type,
      .init = init,
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_use(bool is_pub, bool keyword_consumed) {
  const usize mark = pos_;
  if (!keyword_consumed && !expect(lexer::TokenKind::Use, "use")) {
    return ast::ItemIdx::invalid();
  }
  ast::PathIdx path = parse_path();
  if (!path.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Use;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemUse{
      .path = path,
      .has_alias = false,
      .alias = ast::Ident{},
  });
  if (match(lexer::TokenKind::As)) {
    base::Result<ast::Ident, diag::Reported> alias = parse_ident("alias");
    if (alias.is_err()) {
      return ast::ItemIdx::invalid();
    }
    node.payload.set(ast::ItemUse{
        .path = node.payload.get<ast::ItemUse>().path,
        .has_alias = true,
        .alias = std::move(alias).unwrap(),
    });
  }
  if (!expect(lexer::TokenKind::Semicolon, "`;`")) {
    return ast::ItemIdx::invalid();
  }
  return ast_.items.push_back(node);
}

bool Parser::consume_gt() {
  if (banked_gt_ > 0) {
    --banked_gt_;
    return true;
  }
  if (check(lexer::TokenKind::Greater)) {
    advance();
    return true;
  }
  if (check(lexer::TokenKind::GreaterGreater)) {
    advance();
    banked_gt_ = 1;
    return true;
  }
  return false;
}

bool Parser::parse_decimal_u64(u64* out) {
  const diag::Span span = peek().span;
  const std::string_view spelling = bytes_.substr(span.offset, span.length);
  u64 value = 0;
  for (char c : spelling) {
    if (c == '_') {
      continue;
    }
    if (c < '0' || c > '9') {
      const u32 index = bag_.emit<i18n::Key::ParserArrayLengthNotDecimal>(
          DiagCode::UnexpectedToken, span);
      (void)index;
      return false;
    }
    // Reject rather than wrap: a wrapped length would silently become a
    // smaller array, or zero.
    const u64 digit = static_cast<u64>(c - '0');
    if (value > (~0ull - digit) / 10) {
      const u32 index = bag_.emit<i18n::Key::ParserArrayLengthTooLarge>(
          DiagCode::UnexpectedToken, span);
      (void)index;
      return false;
    }
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

void emit_nesting_too_deep(diag::DiagBag& bag, diag::Span span, u32 limit) {
  const u32 index =
      bag.emit<i18n::Key::ParserNestingTooDeep>(DiagCode::TooDeep, span, limit);
  (void)index;
}

void Parser::report_exhausted_nesting(diag::Span span) {
  if (!reported_too_deep_) {
    reported_too_deep_ = true;
    emit_nesting_too_deep(bag_, span, nesting_.limit());
  }
}

ast::BlockIdx Parser::parse_header_block() {
  check_header_brace();
  return parse_block();
}

bool Parser::expect_header_lbrace() {
  check_header_brace();
  return expect(lexer::TokenKind::LBrace, "`{`");
}

void Parser::check_header_brace() {
  if (peek_kind() != lexer::TokenKind::LBrace || !next_token_on_new_line()) {
    return;
  }
  const u32 index = bag_.emit<i18n::Key::ParserBlockBraceOnHeader>(
      DiagCode::UnexpectedToken, peek().span);
  (void)index;
}

bool Parser::next_token_on_new_line() const {
  if (at_end()) {
    return false;
  }
  usize at = static_cast<usize>(previous_span_.offset) + previous_span_.length;
  const usize end = peek().span.offset;
  if (end > bytes_.size() || at > end) {
    return false;
  }
  // Only whitespace and comments may stand in the gap; a newline
  // outside a comment is what breaks the line. A block comment may
  // span lines without ending the brace's line, which is how the
  // scanner reads it too.
  while (at < end) {
    const char c = bytes_[at];
    if (c == '\n') {
      return true;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++at;
      continue;
    }
    if (c == '/' && at + 1 < end && bytes_[at + 1] == '/') {
      at += 2;
      while (at < end && bytes_[at] != '\n') {
        ++at;
      }
      continue;
    }
    if (c == '/' && at + 1 < end && bytes_[at + 1] == '*') {
      u32 depth = 1;
      at += 2;
      while (at < end && depth > 0) {
        if (bytes_[at] == '/' && at + 1 < end && bytes_[at + 1] == '*') {
          ++depth;
          at += 2;
        } else if (bytes_[at] == '*' && at + 1 < end && bytes_[at + 1] == '/') {
          --depth;
          at += 2;
        } else {
          ++at;
        }
      }
      continue;
    }
    // Something else stands in the gap, so the line cannot be judged
    // here; the parser's own error is the better report.
    return false;
  }
  return false;
}

ast::BlockIdx Parser::parse_block() {
  if (ast_.exhausted()) {
    return ast::BlockIdx::invalid();
  }
  if (nesting_exhausted(peek().span)) {
    return ast::BlockIdx::invalid();
  }
  const base::NestingScope scope(nesting_);
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::BlockIdx::invalid();
  }
  std::vector<ast::StmtIdx> statements;
  ast::ExprIdx value = ast::ExprIdx::invalid();
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::StmtIdx stmt = parse_stmt();
    if (!stmt.is_valid()) {
      if (ast_.exhausted()) {
        break;
      }
      synchronize();
      continue;
    }
    if (ast_.stmts[stmt].kind == ast::StmtKind::Expr &&
        (check(lexer::TokenKind::RBrace) || at_end() ||
         (check(lexer::TokenKind::Semicolon) &&
          (pos_ + 1 >= tokens_.size() ||
           tokens_[pos_ + 1].kind == lexer::TokenKind::RBrace ||
           tokens_[pos_ + 1].kind == lexer::TokenKind::Eof)))) {
      value = ast_.stmts[stmt].payload.get<ast::StmtExpr>().value;
      match(lexer::TokenKind::Semicolon);
      break;
    }
    statements.push_back(stmt);
    if (!match(lexer::TokenKind::Semicolon) &&
        !check(lexer::TokenKind::RBrace) && !at_end()) {
      const u32 index = bag_.emit<i18n::Key::ParserExpectedSemicolon>(
          DiagCode::UnexpectedToken, peek().span);
      (void)index;
      synchronize();
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::BlockIdx::invalid();
  }
  ast::Block block;
  block.span = span_from(mark);
  block.statements = ast::copy_to_arena(ast_.spans, statements);
  block.value = value;
  return ast_.blocks.push_back(block);
}

std::optional<bool> Parser::spelled_equals(usize first,
                                           bool require_ascription) const {
  using K = lexer::TokenKind;
  bool ascription = false;
  i32 depth = 0;
  for (usize ahead = first;; ++ahead) {
    const K kind = peek_kind(ahead);
    if (kind == K::Eof) {
      break;
    }
    if (depth == 0) {
      if (kind == K::Semicolon || kind == K::RBrace || kind == K::ColonEq) {
        break;
      }
      if (kind == K::Eq) {
        if (require_ascription && !ascription) {
          break;
        }
        return ascription;
      }
      if (kind == K::Colon) {
        ascription = true;
      }
    }
    if (kind == K::LParen || kind == K::LBracket || kind == K::LBrace) {
      ++depth;
    } else if ((kind == K::RParen || kind == K::RBracket ||
                kind == K::RBrace) &&
               depth > 0) {
      --depth;
    }
  }
  return std::nullopt;
}

std::optional<ast::StmtIdx> Parser::parse_spelled_declaration(usize mark) {
  using K = lexer::TokenKind;
  const lexer::Token start = peek();
  const std::string_view word =
      bytes_.substr(start.span.offset, start.span.length);
  bool keyword = false;     // `let`/`var`, consumed before the pattern
  bool type_first = false;  // the type token leads the line
  bool matched = false;
  if (start.kind == K::Ident && (word == "let" || word == "var")) {
    // `let a ...` / `var a ...`: a binding named let/var has a
    // separator next, not a pattern.
    const K after = peek_kind(1);
    if (after == K::Mut || after == K::Ident || after == K::Underscore) {
      if (word == "let") {
        const u32 index = bag_.emit<i18n::Key::ParserLetSpelling>(
            DiagCode::Spelling, start.span);
        (void)index;
      } else {
        const u32 index = bag_.emit<i18n::Key::ParserVarSpelling>(
            DiagCode::Spelling, start.span);
        (void)index;
      }
      keyword = true;
      matched = true;
    }
  } else if (start.kind == K::Mut && peek_kind(1) == K::Ident) {
    // `mut a = ...` and `mut a: T = ...`: the mutable spelling wrote
    // `=` for `:=`, with or without an ascription of its own. The
    // ascription decides which form of the declaration to name.
    const std::optional<bool> ascription = spelled_equals(2, false);
    if (ascription.has_value()) {
      if (*ascription) {
        const u32 index = bag_.emit<i18n::Key::ParserTypedDeclarationSpelling>(
            DiagCode::Spelling, start.span);
        (void)index;
      } else {
        const u32 index = bag_.emit<i18n::Key::ParserMutDeclarationSpelling>(
            DiagCode::Spelling, start.span);
        (void)index;
      }
      matched = true;
    }
  } else if (is_primitive_type_token(start.kind) && peek_kind(1) == K::Ident &&
             (peek_kind(2) == K::Eq || peek_kind(2) == K::ColonEq)) {
    const u32 index = bag_.emit<i18n::Key::ParserTypeFirstDeclarationSpelling>(
        DiagCode::Spelling, start.span);
    (void)index;
    matched = true;
    type_first = true;
  } else if (start.kind == K::Ident) {
    // `a: T = ...`: a typed declaration that wrote `=` for `:=`. A
    // plain `a = ...` has no ascription and stays an ordinary
    // reassignment for the analyzer to judge.
    if (spelled_equals(1, true).has_value()) {
      const u32 index = bag_.emit<i18n::Key::ParserTypedDeclarationSpelling>(
          DiagCode::Spelling, start.span);
      (void)index;
      matched = true;
    }
  }
  if (!matched) {
    return std::nullopt;
  }
  if (keyword) {
    advance();  // `let`/`var`
  }
  ast::TypeIdx type = ast::TypeIdx::invalid();
  if (type_first) {
    type = parse_closed_type();
    if (!type.is_valid()) {
      return ast::StmtIdx::invalid();
    }
  }
  const ast::PatternIdx pattern = parse_pattern();
  if (!pattern.is_valid()) {
    return ast::StmtIdx::invalid();
  }
  if (!type_first && match(K::Colon)) {
    type = parse_closed_type();
    if (!type.is_valid()) {
      return ast::StmtIdx::invalid();
    }
  }
  if (!match(K::ColonEq) && !match(K::Eq)) {
    expect(K::ColonEq, "`:=`");
    return ast::StmtIdx::invalid();
  }
  const ast::ExprIdx init = parse_expr();
  if (!init.is_valid()) {
    return ast::StmtIdx::invalid();
  }
  ast::StmtNode node;
  node.kind = ast::StmtKind::Decl;
  node.span = span_from(mark);
  node.payload.set(ast::StmtDecl{
      .pattern = pattern,
      .type = type,
      .init = init,
      .is_const = false,
  });
  return ast_.stmts.push_back(node);
}

std::optional<ast::StmtIdx> Parser::parse_spelled_return(usize mark) {
  using K = lexer::TokenKind;
  const lexer::Token start = peek();
  if (start.kind != K::Ident ||
      bytes_.substr(start.span.offset, start.span.length) != "return") {
    return std::nullopt;
  }
  switch (peek_kind(1)) {
    case K::Integer:
    case K::Float:
    case K::String:
    case K::Char:
    case K::True:
    case K::False:
    case K::Ident:
    case K::If:
    case K::Match:
    case K::Loop:
    case K::While:
    case K::For:
    case K::Ret:
    case K::Unsafe:
    case K::Comp: break;
    default: return std::nullopt;
  }
  const u32 index = bag_.emit<i18n::Key::ParserReturnSpelling>(
      DiagCode::Spelling, start.span);
  (void)index;
  advance();  // `return`
  const ast::ExprIdx value = parse_expr();
  if (!value.is_valid()) {
    return ast::StmtIdx::invalid();
  }
  ast::ExprNode ret_node;
  ret_node.kind = ast::ExprKind::Return;
  ret_node.span = span_from(mark);
  ret_node.payload.set(ast::ExprReturn{
      .value = value,
  });
  const ast::ExprIdx ret_expr = ast_.exprs.push_back(ret_node);
  ast::StmtNode node;
  node.kind = ast::StmtKind::Expr;
  node.span = span_from(mark);
  node.payload.set(ast::StmtExpr{
      .value = ret_expr,
  });
  return ast_.stmts.push_back(node);
}

ast::StmtIdx Parser::parse_stmt() {
  if (ast_.exhausted()) {
    return ast::StmtIdx::invalid();
  }
  const usize mark = pos_;
  // A line spelled like a neighboring language's construct is parsed as
  // the construct it meant, so the report is the spelling and not a
  // cascade.
  if (const std::optional<ast::StmtIdx> spelled =
          parse_spelled_declaration(mark)) {
    return *spelled;
  }
  if (const std::optional<ast::StmtIdx> spelled = parse_spelled_return(mark)) {
    return *spelled;
  }
  // Control-flow heads own their `:=` (if-let/while-let conditions);
  // scanning for a declaration lead would misread them as patterns, and
  // the scan would walk the construct only for the answer to be dropped.
  StmtLead lead = StmtLead::None;
  switch (peek_kind()) {
    case lexer::TokenKind::If:
    case lexer::TokenKind::While:
    case lexer::TokenKind::Loop:
    case lexer::TokenKind::Match:
    case lexer::TokenKind::For: break;
    default: lead = scan_lead(); break;
  }
  if (lead == StmtLead::Decl) {
    // `comp` never declares a value. Parse the line as the
    // const declaration it should have been, so the report is the
    // spelling and not a cascade.
    const lexer::Token comp_token = peek();
    const bool comp_decl = match(lexer::TokenKind::Comp);
    if (comp_decl) {
      const u32 index = bag_.emit<i18n::Key::ParserCompDeclarationSpelling>(
          DiagCode::Spelling, comp_token.span);
      (void)index;
    }
    const bool is_const = match(lexer::TokenKind::Const) || comp_decl;
    const ast::PatternIdx pattern = parse_pattern();
    if (!pattern.is_valid()) {
      return ast::StmtIdx::invalid();
    }
    ast::TypeIdx type = ast::TypeIdx::invalid();
    if (match(lexer::TokenKind::Colon)) {
      type = parse_closed_type();
      if (!type.is_valid()) {
        return ast::StmtIdx::invalid();
      }
    }
    if (!expect(lexer::TokenKind::ColonEq, "`:=`")) {
      return ast::StmtIdx::invalid();
    }
    const ast::ExprIdx init = parse_expr();
    if (!init.is_valid()) {
      return ast::StmtIdx::invalid();
    }
    ast::StmtNode node;
    node.kind = ast::StmtKind::Decl;
    node.span = span_from(mark);
    node.payload.set(ast::StmtDecl{
        .pattern = pattern,
        .type = type,
        .init = init,
        .is_const = is_const,
    });
    return ast_.stmts.push_back(node);
  }
  if (lead == StmtLead::Reassign) {
    ast::ExprIdx place = parse_expr();
    if (!place.is_valid()) {
      return ast::StmtIdx::invalid();
    }
    ast::BinaryOp op = ast::BinaryOp::Add;
    bool compound = false;
    if (match(lexer::TokenKind::Eq)) {
      compound = false;
    } else if (is_compound_assign(peek_kind())) {
      op = compound_op(peek_kind());
      compound = true;
      advance();
    } else {
      expect(lexer::TokenKind::Eq, "`=`");
      return ast::StmtIdx::invalid();
    }
    ast::ExprIdx value = parse_expr();
    if (!value.is_valid()) {
      return ast::StmtIdx::invalid();
    }
    ast::StmtNode node;
    node.kind = ast::StmtKind::Reassign;
    node.span = span_from(mark);
    node.payload.set(ast::StmtReassign{
        .place = place,
        .compound = compound,
        .op = op,
        .value = value,
    });
    return ast_.stmts.push_back(node);
  }
  ast::ExprIdx value = parse_expr();
  if (!value.is_valid()) {
    return ast::StmtIdx::invalid();
  }
  ast::StmtNode node;
  node.kind = ast::StmtKind::Expr;
  node.span = span_from(mark);
  node.payload.set(ast::StmtExpr{
      .value = value,
  });
  return ast_.stmts.push_back(node);
}

}  // namespace parser
