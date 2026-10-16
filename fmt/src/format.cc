// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/src/format.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fmt/src/cst.h"
#include "fmt/src/doc.h"
#include "fmt/src/lex.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"

namespace alcy_fmt {

namespace {

using lexer::TokenKind;

std::string_view node_text(std::string_view source, const Node& node) {
  return text_of(source, node.offset, node.length);
}

bool is_path_like(TokenKind kind) {
  switch (kind) {
    case TokenKind::Ident:
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

bool is_prefix_operator(TokenKind kind) {
  switch (kind) {
    case TokenKind::Minus:
    case TokenKind::Bang:
    case TokenKind::Tilde:
    case TokenKind::Amp:
    case TokenKind::Star:
    case TokenKind::StarStar:
    case TokenKind::Hash: return true;
    default: return false;
  }
}

// Whether a token in front of an expression lets a following prefix
// operator read as a prefix rather than a binary one.
bool allows_prefix_after(TokenKind kind) {
  switch (kind) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::StarStar:
    case TokenKind::Amp:
    case TokenKind::Pipe:
    case TokenKind::Caret:
    case TokenKind::Tilde:
    case TokenKind::LessLess:
    case TokenKind::GreaterGreater:
    case TokenKind::PlusEq:
    case TokenKind::MinusEq:
    case TokenKind::StarEq:
    case TokenKind::SlashEq:
    case TokenKind::PercentEq:
    case TokenKind::StarStarEq:
    case TokenKind::AmpEq:
    case TokenKind::PipeEq:
    case TokenKind::CaretEq:
    case TokenKind::LessLessEq:
    case TokenKind::GreaterGreaterEq:
    case TokenKind::ColonEq:
    case TokenKind::Eq:
    case TokenKind::EqEq:
    case TokenKind::Bang:
    case TokenKind::AmpAmp:
    case TokenKind::PipePipe:
    case TokenKind::BangEq:
    case TokenKind::Greater:
    case TokenKind::Less:
    case TokenKind::GreaterEq:
    case TokenKind::LessEq:
    case TokenKind::Arrow:
    case TokenKind::FatArrow:
    case TokenKind::Colon:
    case TokenKind::Comma:
    case TokenKind::As:
    case TokenKind::In:
    case TokenKind::Ret:
    case TokenKind::DotDot:
    case TokenKind::DotDotEq:
    case TokenKind::DotDotLess: return true;
    default: return false;
  }
}

// Whether a line may break after this operator. The grammar continues an
// expression at a newline only when the line it leaves cannot end a
// statement, so a break belongs after an operator rather than before one.
bool is_break_after(TokenKind kind) {
  switch (kind) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::StarStar:
    case TokenKind::Amp:
    case TokenKind::Pipe:
    case TokenKind::Caret:
    case TokenKind::LessLess:
    case TokenKind::GreaterGreater:
    case TokenKind::PlusEq:
    case TokenKind::MinusEq:
    case TokenKind::StarEq:
    case TokenKind::SlashEq:
    case TokenKind::PercentEq:
    case TokenKind::StarStarEq:
    case TokenKind::AmpEq:
    case TokenKind::PipeEq:
    case TokenKind::CaretEq:
    case TokenKind::LessLessEq:
    case TokenKind::GreaterGreaterEq:
    case TokenKind::ColonEq:
    case TokenKind::Eq:
    case TokenKind::EqEq:
    case TokenKind::AmpAmp:
    case TokenKind::PipePipe:
    case TokenKind::BangEq:
    case TokenKind::Greater:
    case TokenKind::Less:
    case TokenKind::GreaterEq:
    case TokenKind::LessEq:
    case TokenKind::Arrow:
    case TokenKind::FatArrow:
    case TokenKind::As:
    case TokenKind::In: return true;
    default: return false;
  }
}

// Turns the tree into a layout document. The printer owns no state
// between calls: every decision comes from the tree and the injected
// config.
class Printer {
 public:
  Printer(std::string_view source, const FormatConfig& config)
      : source_(source), config_(config) {}

  Doc run(const Node& root) {
    return print_body(root, BraceRole::Block, true);
  }

 private:
  enum class Sep : u8 { None, Space, Line, SoftLine };

  struct Piece {
    Doc doc;
    Sep sep = Sep::None;
  };

  struct Item {
    std::vector<const Node*> nodes;
  };

  struct Chunk {
    std::vector<const Node*> nodes;
    bool is_comment = false;
    usize newlines_before = 0;
    // The chunk ended at a semicolon spelled in the source rather than
    // at the newline the lexer turned into one.
    bool explicit_semi = false;
  };

  Doc print_body(const Node& seq, BraceRole role, bool top_level);
  std::vector<Chunk> collect_chunks(const Node& seq, BraceRole role) const;
  Doc print_chunk(const Chunk& chunk, BraceRole role, bool last_chunk);
  static bool ends_like_statement(const std::vector<const Node*>& nodes);
  Doc print_line(const std::vector<const Node*>& nodes);
  Doc print_flow(const std::vector<const Node*>& nodes, bool allow_wrap);
  Doc build_flow(std::vector<Piece> pieces, bool allow_wrap);
  Doc node_doc(const Node& node);
  Doc print_group(const Node& group);
  Doc print_brace_body(const Node& group);
  Doc print_list(const Node& group, std::string_view open, std::string_view close);
  Doc print_literal(const Node& group);
  std::vector<Item> split_on_commas(const Node& seq,
                                    bool& had_comma,
                                    bool& trailing) const;
  bool space_before(const Node* prev,
                    bool prev_generic,
                    bool prev_unary,
                    const Node& cur,
                    bool cur_generic) const;
  void compute_generic(const std::vector<const Node*>& nodes,
                       std::vector<bool>& generic) const;
  bool looks_like_generic(const std::vector<const Node*>& nodes, usize i) const;
  static const Node* previous_significant(const std::vector<const Node*>& nodes,
                                          usize i);
  static const Node* next_significant(const std::vector<const Node*>& nodes,
                                      usize i);
  static bool chunk_is_use(const Chunk& chunk);

  std::string_view source_;
  FormatConfig config_;
};

const Node* Printer::previous_significant(const std::vector<const Node*>& nodes,
                                          usize i) {
  while (i > 0) {
    --i;
    if (nodes[i]->kind != NodeKind::Comment) {
      return nodes[i];
    }
  }
  return nullptr;
}

const Node* Printer::next_significant(const std::vector<const Node*>& nodes,
                                      usize i) {
  while (i < nodes.size()) {
    if (nodes[i]->kind != NodeKind::Comment) {
      return nodes[i];
    }
    ++i;
  }
  return nullptr;
}

bool Printer::space_before(const Node* prev,
                           bool prev_generic,
                           bool prev_unary,
                           const Node& cur,
                           bool cur_generic) const {
  if (prev == nullptr) {
    return false;
  }
  if (cur.kind == NodeKind::Token) {
    switch (cur.token) {
      case TokenKind::Comma:
      case TokenKind::Semicolon:
      case TokenKind::Dot:
      case TokenKind::ColonColon:
      case TokenKind::Question:
      case TokenKind::Colon:
      case TokenKind::DotDot:
      case TokenKind::DotDotEq:
      case TokenKind::DotDotLess:
      case TokenKind::RParen:
      case TokenKind::RBracket:
      case TokenKind::RBrace: return false;
      case TokenKind::Less:
      case TokenKind::Greater:
      case TokenKind::GreaterGreater:
        if (cur_generic) {
          return false;
        }
        break;
      default: break;
    }
  }
  if (cur.kind == NodeKind::Group &&
      (cur.group_role == GroupRole::Paren ||
       cur.group_role == GroupRole::Bracket)) {
    if (prev->kind == NodeKind::Group &&
        (prev->group_role == GroupRole::Paren ||
         prev->group_role == GroupRole::Bracket)) {
      return false;
    }
    if (prev->kind == NodeKind::Token) {
      if (is_path_like(prev->token)) {
        return false;
      }
      if ((prev->token == TokenKind::Greater ||
           prev->token == TokenKind::GreaterGreater) &&
          prev_generic) {
        return false;
      }
      if (prev->token == TokenKind::ColonColon) {
        return false;
      }
    }
  }
  if (prev->kind == NodeKind::Token) {
    switch (prev->token) {
      case TokenKind::Dot:
      case TokenKind::ColonColon:
      case TokenKind::Question:
      case TokenKind::DotDot:
      case TokenKind::DotDotEq:
      case TokenKind::DotDotLess: return false;
      case TokenKind::Less:
      case TokenKind::Greater:
      case TokenKind::GreaterGreater:
        // A closing bracket of a generic list touches what it closes
        // over, except a brace: a greater-than then an opening brace is
        // a header or a literal, and both read with the space.
        if (prev_generic &&
            !(cur.kind == NodeKind::Group &&
              cur.group_role == GroupRole::Brace)) {
          return false;
        }
        break;
      default: break;
    }
    if (prev_unary) {
      return false;
    }
  }
  return true;
}

void Printer::compute_generic(const std::vector<const Node*>& nodes,
                              std::vector<bool>& generic) const {
  usize depth = 0;
  for (usize i = 0; i < nodes.size(); ++i) {
    const Node& node = *nodes[i];
    if (node.kind != NodeKind::Token) {
      continue;
    }
    if (node.token == TokenKind::Less) {
      if (looks_like_generic(nodes, i)) {
        generic[i] = true;
        ++depth;
      }
    } else if (node.token == TokenKind::Greater) {
      if (depth > 0) {
        generic[i] = true;
        --depth;
      }
    } else if (node.token == TokenKind::GreaterGreater) {
      if (depth >= 2) {
        generic[i] = true;
        depth -= 2;
      } else if (depth == 1) {
        generic[i] = true;
        depth = 0;
      }
    }
  }
}

bool Printer::looks_like_generic(const std::vector<const Node*>& nodes,
                                 usize i) const {
  const Node* prev = previous_significant(nodes, i);
  if (prev == nullptr || prev->kind != NodeKind::Token) {
    return false;
  }
  const bool impl_params = prev->token == TokenKind::Impl;
  const bool prev_ok = is_path_like(prev->token) ||
                       prev->token == TokenKind::ColonColon ||
                       prev->token == TokenKind::Greater ||
                       prev->token == TokenKind::GreaterGreater || impl_params;
  if (!prev_ok) {
    return false;
  }
  usize depth = 1;
  usize j = i + 1;
  for (; j < nodes.size(); ++j) {
    const Node& node = *nodes[j];
    if (node.kind == NodeKind::Comment) {
      continue;
    }
    if (node.kind == NodeKind::Group) {
      continue;
    }
    switch (node.token) {
      case TokenKind::Less: ++depth; break;
      case TokenKind::Greater:
        if (depth > 0) {
          --depth;
        }
        break;
      case TokenKind::GreaterGreater: depth = depth >= 2 ? depth - 2 : 0; break;
      case TokenKind::Semicolon:
      case TokenKind::Eq:
      case TokenKind::ColonEq:
      case TokenKind::AmpAmp:
      case TokenKind::PipePipe:
      case TokenKind::If:
      case TokenKind::Else:
      case TokenKind::Match:
      case TokenKind::While:
      case TokenKind::For:
      case TokenKind::Loop:
      case TokenKind::Fn:
      case TokenKind::Ret:
      case TokenKind::Struct:
      case TokenKind::Enum:
      case TokenKind::Impl:
      case TokenKind::Spec: return false;
      default: break;
    }
    if (depth == 0) {
      ++j;
      break;
    }
  }
  if (depth != 0) {
    return false;
  }
  const Node* follower = next_significant(nodes, j);
  if (follower == nullptr) {
    return true;
  }
  if (follower->kind == NodeKind::Group) {
    return true;
  }
  if (impl_params && follower->token == TokenKind::Ident) {
    return true;
  }
  switch (follower->token) {
    case TokenKind::Comma:
    case TokenKind::RParen:
    case TokenKind::RBracket:
    case TokenKind::RBrace:
    case TokenKind::ColonColon:
    case TokenKind::Dot:
    case TokenKind::Question:
    case TokenKind::Semicolon:
    case TokenKind::Greater:
    case TokenKind::GreaterGreater:
    case TokenKind::Arrow:
    case TokenKind::FatArrow:
    case TokenKind::As:
    case TokenKind::Eq:
    case TokenKind::ColonEq:
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::StarStar:
    case TokenKind::Amp:
    case TokenKind::Pipe:
    case TokenKind::Caret:
    case TokenKind::LessLess:
    case TokenKind::EqEq:
    case TokenKind::BangEq:
    case TokenKind::GreaterEq:
    case TokenKind::LessEq:
    case TokenKind::AmpAmp:
    case TokenKind::PipePipe:
    case TokenKind::PlusEq:
    case TokenKind::MinusEq:
    case TokenKind::StarEq:
    case TokenKind::SlashEq:
    case TokenKind::PercentEq:
    case TokenKind::StarStarEq:
    case TokenKind::AmpEq:
    case TokenKind::PipeEq:
    case TokenKind::CaretEq:
    case TokenKind::LessLessEq:
    case TokenKind::GreaterGreaterEq: return true;
    default: return false;
  }
}

Doc Printer::build_flow(std::vector<Piece> pieces, bool allow_wrap) {
  usize first_break = pieces.size();
  for (usize i = 0; i < pieces.size(); ++i) {
    if (pieces[i].sep == Sep::Line || pieces[i].sep == Sep::SoftLine) {
      first_break = i;
      break;
    }
  }
  if (!allow_wrap || first_break == pieces.size()) {
    std::vector<Doc> out;
    for (Piece& piece : pieces) {
      if (piece.sep == Sep::Space || piece.sep == Sep::Line) {
        out.push_back(doc_text(" "));
      }
      out.push_back(std::move(piece.doc));
    }
    return doc_concat(std::move(out));
  }
  std::vector<Doc> head;
  for (usize i = 0; i < first_break; ++i) {
    if (pieces[i].sep == Sep::Space) {
      head.push_back(doc_text(" "));
    }
    head.push_back(std::move(pieces[i].doc));
  }
  std::vector<Doc> tail;
  for (usize i = first_break; i < pieces.size(); ++i) {
    switch (pieces[i].sep) {
      case Sep::Space: tail.push_back(doc_text(" ")); break;
      case Sep::Line: tail.push_back(doc_line()); break;
      case Sep::SoftLine: tail.push_back(doc_softline()); break;
      case Sep::None: break;
    }
    tail.push_back(std::move(pieces[i].doc));
  }
  std::vector<Doc> out;
  out.push_back(doc_concat(std::move(head)));
  out.push_back(doc_nest(config_.indent_width, doc_concat(std::move(tail))));
  return doc_group(doc_concat(std::move(out)));
}

Doc Printer::print_flow(const std::vector<const Node*>& nodes, bool allow_wrap) {
  std::vector<bool> generic(nodes.size(), false);
  compute_generic(nodes, generic);
  std::vector<bool> unary(nodes.size(), false);
  for (usize i = 0; i < nodes.size(); ++i) {
    const Node& node = *nodes[i];
    if (node.kind == NodeKind::Token && is_prefix_operator(node.token)) {
      const Node* prev = previous_significant(nodes, i);
      unary[i] = prev == nullptr ||
                 (prev->kind == NodeKind::Token &&
                  allows_prefix_after(prev->token));
    }
  }

  std::vector<Piece> pieces;
  const Node* prev = nullptr;
  bool prev_generic = false;
  bool prev_unary = false;
  bool pending_break = false;
  bool suppress = false;
  for (usize i = 0; i < nodes.size(); ++i) {
    const Node& node = *nodes[i];
    if (node.kind == NodeKind::Comment) {
      Sep sep = Sep::None;
      if (!suppress && !pieces.empty()) {
        sep = Sep::Space;
      }
      Doc text = doc_text(node_text(source_, node));
      // A comment that runs to end of line ends the line it sits on, so
      // what follows it has to start a new one. A block comment does not:
      // code may keep reading after it on the same line.
      const bool runs_to_eol = node.comment != CommentKind::Block;
      if (runs_to_eol) {
        std::vector<Doc> parts;
        parts.push_back(std::move(text));
        parts.push_back(doc_hardline());
        text = doc_concat(std::move(parts));
        suppress = true;
      } else {
        suppress = false;
      }
      pieces.push_back(Piece{std::move(text), sep});
      pending_break = false;
      continue;
    }
    Sep sep = Sep::None;
    if (pieces.empty()) {
      sep = Sep::None;
    } else if (suppress) {
      sep = Sep::None;
      suppress = false;
    } else if (pending_break) {
      sep = allow_wrap ? Sep::Line : Sep::Space;
    } else if (node.kind == NodeKind::Token && node.token == TokenKind::Dot) {
      sep = allow_wrap ? Sep::SoftLine : Sep::None;
    } else if (space_before(prev, prev_generic, prev_unary, node, generic[i])) {
      sep = Sep::Space;
    }
    pieces.push_back(Piece{node_doc(node), sep});
    // A generic bracket reads as a delimiter here, not as a comparison:
    // a break inside an argument list would space it flat.
    pending_break = node.kind == NodeKind::Token && !unary[i] &&
                   !generic[i] && is_break_after(node.token);
    prev = &node;
    prev_generic = generic[i];
    prev_unary = unary[i];
  }
  return build_flow(std::move(pieces), allow_wrap);
}

Doc Printer::node_doc(const Node& node) {
  switch (node.kind) {
    case NodeKind::Token: return doc_text(node_text(source_, node));
    case NodeKind::Comment: return doc_text(node_text(source_, node));
    case NodeKind::Group: return print_group(node);
    case NodeKind::Sequence: return doc_empty();
  }
  return doc_empty();
}

Doc Printer::print_group(const Node& group) {
  switch (group.group_role) {
    case GroupRole::Paren: return print_list(group, "(", ")");
    case GroupRole::Bracket: return print_list(group, "[", "]");
    case GroupRole::Brace:
      if (group.brace_role == BraceRole::Literal) {
        return print_literal(group);
      }
      return print_brace_body(group);
  }
  return doc_empty();
}

Doc Printer::print_brace_body(const Node& group) {
  const std::string close = group.has_closer ? "}" : "";
  const Doc body = group.kids.empty()
                       ? doc_empty()
                       : print_body(group.kids[0], group.brace_role, false);
  std::vector<Doc> parts;
  parts.push_back(doc_text("{"));
  if (body.kind == DocKind::Empty) {
    parts.push_back(doc_hardline());
  } else {
    std::vector<Doc> inner;
    inner.push_back(doc_hardline());
    inner.push_back(body);
    parts.push_back(
        doc_nest(config_.indent_width, doc_concat(std::move(inner))));
    parts.push_back(doc_hardline());
  }
  parts.push_back(doc_text(close));
  return doc_concat(std::move(parts));
}

std::vector<Printer::Item> Printer::split_on_commas(const Node& seq,
                                                    bool& had_comma,
                                                    bool& trailing) const {
  std::vector<Item> items;
  Item current;
  had_comma = false;
  trailing = false;
  for (const Node& node : seq.kids) {
    if (node.kind == NodeKind::Token && node.token == TokenKind::Comma) {
      had_comma = true;
      items.push_back(std::move(current));
      current = Item{};
      continue;
    }
    current.nodes.push_back(&node);
  }
  items.push_back(std::move(current));
  if (had_comma && items.back().nodes.empty()) {
    trailing = true;
    items.pop_back();
  }
  return items;
}

Doc Printer::print_list(const Node& group,
                        std::string_view open,
                        std::string_view close) {
  if (group.kids.empty()) {
    std::vector<Doc> parts;
    parts.push_back(doc_text(open));
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }
  const Node& inner = group.kids[0];
  bool had_comma = false;
  bool trailing = false;
  std::vector<Item> items = split_on_commas(inner, had_comma, trailing);
  if (!group.has_closer) {
    std::vector<const Node*> nodes;
    for (const Node& node : inner.kids) {
      nodes.push_back(&node);
    }
    std::vector<Doc> parts;
    parts.push_back(doc_text(open));
    parts.push_back(print_flow(nodes, true));
    return doc_concat(std::move(parts));
  }
  if (items.empty() || (items.size() == 1 && items[0].nodes.empty())) {
    std::vector<Doc> parts;
    parts.push_back(doc_text(open));
    if (had_comma) {
      parts.push_back(doc_text(","));
    }
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }

  std::vector<Doc> item_docs;
  item_docs.reserve(items.size());
  for (Item& item : items) {
    item_docs.push_back(print_flow(item.nodes, true));
  }
  std::vector<Doc> body;
  for (usize k = 0; k < item_docs.size(); ++k) {
    if (k > 0) {
      body.push_back(doc_text(","));
      body.push_back(doc_line());
    }
    body.push_back(std::move(item_docs[k]));
  }
  if (trailing || items.size() >= 2) {
    body.push_back(
        doc_if_break(doc_text(","), trailing ? doc_text(",") : doc_empty()));
  }
  std::vector<Doc> inner_parts;
  inner_parts.push_back(doc_softline());
  inner_parts.push_back(doc_concat(std::move(body)));
  std::vector<Doc> out;
  out.push_back(doc_text(open));
  out.push_back(
      doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
  out.push_back(doc_softline());
  out.push_back(doc_text(close));
  return doc_group(doc_concat(std::move(out)));
}

Doc Printer::print_literal(const Node& group) {
  if (group.kids.empty()) {
    return doc_text("{}");
  }
  const Node& inner = group.kids[0];
  bool had_comma = false;
  bool trailing = false;
  std::vector<Item> items = split_on_commas(inner, had_comma, trailing);
  const std::string close = group.has_closer ? "}" : "";
  if (items.empty() || (items.size() == 1 && items[0].nodes.empty())) {
    if (had_comma) {
      return doc_text("{,}");
    }
    return doc_text("{}");
  }
  if (!had_comma) {
    std::vector<Doc> inner_parts;
    inner_parts.push_back(doc_line());
    inner_parts.push_back(print_flow(items[0].nodes, true));
    std::vector<Doc> out;
    out.push_back(doc_text("{"));
    out.push_back(
        doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
    out.push_back(doc_line());
    out.push_back(doc_text(close));
    return doc_group(doc_concat(std::move(out)));
  }

  std::vector<Doc> item_docs;
  item_docs.reserve(items.size());
  for (Item& item : items) {
    item_docs.push_back(print_flow(item.nodes, true));
  }
  std::vector<Doc> body;
  for (usize k = 0; k < item_docs.size(); ++k) {
    if (k > 0) {
      body.push_back(doc_text(","));
      body.push_back(doc_line());
    }
    body.push_back(std::move(item_docs[k]));
  }
  if (trailing || items.size() >= 2) {
    body.push_back(
        doc_if_break(doc_text(","), trailing ? doc_text(",") : doc_empty()));
  }
  std::vector<Doc> inner_parts;
  inner_parts.push_back(doc_line());
  inner_parts.push_back(doc_concat(std::move(body)));
  std::vector<Doc> out;
  out.push_back(doc_text("{"));
  out.push_back(
      doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
  out.push_back(doc_line());
  out.push_back(doc_text(close));
  return doc_group(doc_concat(std::move(out)));
}

std::vector<Printer::Chunk> Printer::collect_chunks(const Node& seq,
                                                    BraceRole role) const {
  const bool split_comma = role == BraceRole::StructBody ||
                           role == BraceRole::EnumBody ||
                           role == BraceRole::MatchBody;
  std::vector<Chunk> chunks;
  Chunk current;
  const auto flush = [&]() {
    if (!current.nodes.empty()) {
      chunks.push_back(std::move(current));
      current = Chunk{};
    }
  };
  for (const Node& node : seq.kids) {
    if (node.kind == NodeKind::Token && node.token == TokenKind::Semicolon) {
      current.explicit_semi = !node.synthetic_semi;
      flush();
      continue;
    }
    if (split_comma && node.kind == NodeKind::Token &&
        node.token == TokenKind::Comma) {
      flush();
      continue;
    }
    if (node.kind == NodeKind::Comment) {
      const bool same_line = node.newlines_before == 0 && !current.nodes.empty();
      if (same_line) {
        current.nodes.push_back(&node);
        if (node.comment == CommentKind::Line) {
          flush();
        }
      } else {
        flush();
        Chunk comment;
        comment.nodes.push_back(&node);
        comment.is_comment = true;
        comment.newlines_before = node.newlines_before;
        chunks.push_back(std::move(comment));
      }
      continue;
    }
    if (current.nodes.empty()) {
      current.newlines_before = node.newlines_before;
    }
    current.nodes.push_back(&node);
  }
  flush();
  return chunks;
}

// Whether a run of nodes is one the lexer would end a statement after, so
// the newline that replaces a spelled-out semicolon reads back as one.
bool Printer::ends_like_statement(const std::vector<const Node*>& nodes) {
  for (usize i = nodes.size(); i > 0; --i) {
    const Node& node = *nodes[i - 1];
    if (node.kind == NodeKind::Comment) {
      continue;
    }
    if (node.kind == NodeKind::Group) {
      return node.has_closer;
    }
    switch (node.token) {
      case TokenKind::Ident:
      case TokenKind::Underscore:
      case TokenKind::Integer:
      case TokenKind::Float:
      case TokenKind::String:
      case TokenKind::Char:
      case TokenKind::True:
      case TokenKind::False:
      case TokenKind::Break:
      case TokenKind::Continue:
      case TokenKind::Ret:
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
  return false;
}

bool Printer::chunk_is_use(const Chunk& chunk) {
  for (usize i = 0; i < chunk.nodes.size(); ++i) {
    const Node& node = *chunk.nodes[i];
    if (node.kind != NodeKind::Token) {
      continue;
    }
    if (node.token == TokenKind::Use) {
      return true;
    }
    if (node.token == TokenKind::Pub) {
      for (usize j = i + 1; j < chunk.nodes.size(); ++j) {
        if (chunk.nodes[j]->kind == NodeKind::Token &&
            chunk.nodes[j]->token == TokenKind::Use) {
          return true;
        }
      }
      return false;
    }
    return false;
  }
  return false;
}

Doc Printer::print_body(const Node& seq, BraceRole role, bool top_level) {
  const std::vector<Chunk> chunks = collect_chunks(seq, role);
  if (chunks.empty()) {
    return doc_empty();
  }
  const bool member = top_level || role == BraceRole::ImplBody ||
                      role == BraceRole::SpecBody ||
                      role == BraceRole::ExternBody;
  std::vector<Doc> parts;
  for (usize k = 0; k < chunks.size(); ++k) {
    if (k > 0) {
      const bool prev_comment = chunks[k - 1].is_comment;
      const bool cur_comment = chunks[k].is_comment;
      bool blank = false;
      if (member) {
        if (prev_comment && cur_comment) {
          blank = chunks[k].newlines_before >= 2;
        } else if (prev_comment) {
          blank = chunks[k].newlines_before >= 2;
        } else if (cur_comment) {
          blank = true;
        } else {
          blank = true;
        }
        if (top_level && chunk_is_use(chunks[k - 1]) &&
            chunk_is_use(chunks[k])) {
          blank = false;
        }
      } else {
        blank = chunks[k].newlines_before >= 2;
      }
      parts.push_back(doc_hardline());
      if (blank) {
        parts.push_back(doc_hardline());
      }
    }
    parts.push_back(print_chunk(chunks[k], role, k + 1 == chunks.size()));
  }
  return doc_concat(std::move(parts));
}

Doc Printer::print_chunk(const Chunk& chunk, BraceRole role, bool last_chunk) {
  if (chunk.is_comment) {
    return doc_text(node_text(source_, *chunk.nodes.front()));
  }
  // A comment that runs to end of line ends the chunk's line, so it
  // belongs after the chunk's own terminator and outside the flow: kept
  // inside, its newline would force the code in front of it to wrap.
  usize end = chunk.nodes.size();
  while (end > 0) {
    const Node& node = *chunk.nodes[end - 1];
    if (node.kind != NodeKind::Comment ||
        node.comment == CommentKind::Block) {
      break;
    }
    --end;
  }
  std::vector<const Node*> code(
      chunk.nodes.begin(), chunk.nodes.begin() + static_cast<std::ptrdiff_t>(end));
  std::vector<Doc> parts;
  parts.push_back(print_line(code));

  // A spelled-out semicolon survives wherever the newline that replaces
  // it would not read the same. A spec or extern member is terminated
  // by one outright. The last chunk of a body keeps its statement
  // status: without the semicolon the run becomes the block's trailing
  // expression instead of a statement whose value nobody takes. A run
  // the lexer does not end a statement after (an intrinsic signature
  // that closes in a greater-than) keeps it too. Everywhere else the
  // line break reads the same to the parser.
  bool keep_semi = chunk.explicit_semi;
  if (keep_semi && role != BraceRole::SpecBody &&
      role != BraceRole::ExternBody && !last_chunk &&
      ends_like_statement(code)) {
    keep_semi = false;
  }

  bool comma = false;
  if (role == BraceRole::StructBody || role == BraceRole::EnumBody) {
    comma = true;
  } else if (role == BraceRole::MatchBody && !chunk.explicit_semi) {
    const Node* last = nullptr;
    for (usize i = code.size(); i > 0; --i) {
      if (code[i - 1]->kind != NodeKind::Comment) {
        last = code[i - 1];
        break;
      }
    }
    const bool block_body =
        last != nullptr && last->kind == NodeKind::Group &&
        last->group_role == GroupRole::Brace &&
        last->brace_role != BraceRole::Literal;
    comma = !block_body;
  }
  if (comma) {
    parts.push_back(doc_text(","));
  }
  if (keep_semi) {
    parts.push_back(doc_text(";"));
  }
  for (usize i = end; i < chunk.nodes.size(); ++i) {
    parts.push_back(doc_text(" "));
    parts.push_back(doc_text(node_text(source_, *chunk.nodes[i])));
  }
  return doc_concat(std::move(parts));
}

Doc Printer::print_line(const std::vector<const Node*>& nodes) {
  bool has_block = false;
  for (const Node* node : nodes) {
    if (node->kind == NodeKind::Group &&
        node->group_role == GroupRole::Brace &&
        node->brace_role != BraceRole::Literal) {
      has_block = true;
      break;
    }
  }
  return print_flow(nodes, !has_block);
}

}  // namespace

std::string format(std::string_view source, const FormatConfig& config) {
  const std::vector<Lexeme> lexemes = lex(source);
  const Node root = build_cst(lexemes);
  Printer printer(source, config);
  const Doc document = printer.run(root);
  std::string out = render(document, config.line_length);
  while (!out.empty() &&
         (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) {
    out.pop_back();
  }
  if (!out.empty()) {
    out.push_back('\n');
  }
  return out;
}

}  // namespace alcy_fmt
