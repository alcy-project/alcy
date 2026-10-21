// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/formatter.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cst/cst.h"
#include "cst/lex.h"
#include "fmt/doc.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"

namespace alcy_fmt {

namespace {

using lexer::TokenKind;

std::string_view node_text(std::string_view source, const cst::Node& node) {
  return cst::text_of(source, node.offset, node.length);
}

// The bytes of an error token, printed as they were read. A span that
// reaches the end of the source drops trailing newlines: the lexer's
// invalid-scalar clipping absorbs the line break there -- the one the
// formatter writes itself after the last token -- and a span that grew
// by it in one pass and not the next would move the layout with it.
// A span that ends short of the source keeps every byte, because the
// next pass reads those bytes back the same way.
std::string_view error_text(std::string_view source, const cst::Node& node) {
  std::string_view text = cst::text_of(source, node.offset, node.length);
  // Trailing blanks never survive into the output -- the renderer
  // trims them before every newline it writes -- so the measure reads
  // the span without them, or a wrap decision is made over bytes the
  // output does not carry.
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  const usize span_end = node.offset + node.length;
  if (source.find_first_not_of(" \t\r\n", span_end) == std::string_view::npos) {
    // Nothing but blanks follow the span, so the tail of the layout
    // normalizes every one of them away, and the newlines the lexer's
    // clipping absorbed into the span go with them. The line break
    // that lands there is the one the formatter writes itself.
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.remove_suffix(1);
    }
  }
  return text;
}

// Whether an error token's span is still hungry: the lexer stopped it
// at a boundary it did not choose, so whatever the layout writes after
// it joins the span on the next pass. A literal that never closed takes
// bytes to the newline, an unterminated block comment takes the rest of
// the file, and a UTF-8 lead that did not get all the bytes it promised
// takes them from what follows. A complete scalar and a closed literal
// ended where they ended, so a separator after one reads back as a
// separator.
bool error_absorbs(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  const auto first = static_cast<u8>(text.front());
  if (first == '\'' || first == '"') {
    return text.size() == 1 || text.back() != static_cast<char>(first);
  }
  if (first == '/' && text.size() >= 2 && text[1] == '*') {
    return true;
  }
  const bool lead = (first & 0xE0) == 0xC0 || (first & 0xF0) == 0xE0 ||
                    (first & 0xF8) == 0xF0;
  if (lead) {
    const usize promise = (first & 0xE0) == 0xC0   ? 2
                          : (first & 0xF0) == 0xE0 ? 3
                                                   : 4;
    if (text.size() < promise) {
      return true;
    }
  }
  return false;
}

// Whether two tokens printed with nothing between them would read back
// as one: the lexer takes the longest match, so a pair whose joined
// bytes begin a longer spelling merges, and the token count of the
// output would differ from the tree that produced it. A space between
// them keeps the count, and the layout owns the byte it inserted.
bool tokens_merge(std::string_view prev, std::string_view next) {
  static constexpr std::string_view SPELLINGS[] = {
      "..=", "..<", "**=", "<<=", ">>=", "->", "=>", "==", "!=",
      ">=",  "<=",  "&&",  "||",  "**",  "::", ":=", "..", "<<",
      ">>",  "+=",  "-=",  "*=",  "/=",  "%=", "&=", "|=", "^="};
  if (prev.empty() || next.empty()) {
    return false;
  }
  for (std::string_view spelling : SPELLINGS) {
    if (spelling.size() > prev.size() && spelling.starts_with(prev) &&
        spelling[prev.size()] == next.front()) {
      return true;
    }
  }
  return false;
}

// Whether a dot that touches a number reads as the number's decimal
// point: the lexer looks past the dot, and an identifier that is not
// the float suffixes leaves it a member access. Nothing past it, or
// any other byte, continues the number.
bool dot_joins_number(std::string_view after) {
  if (after.empty()) {
    return true;
  }
  const char c = after.front();
  const bool ident =
      (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  if (!ident) {
    return true;
  }
  return after == "f32" || after == "f64";
}

bool is_path_like(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Ident:
    case lexer::TokenKind::Self:
    case lexer::TokenKind::SelfType:
    case lexer::TokenKind::I8:
    case lexer::TokenKind::I16:
    case lexer::TokenKind::I32:
    case lexer::TokenKind::I64:
    case lexer::TokenKind::I128:
    case lexer::TokenKind::Isize:
    case lexer::TokenKind::U8:
    case lexer::TokenKind::U16:
    case lexer::TokenKind::U32:
    case lexer::TokenKind::U64:
    case lexer::TokenKind::U128:
    case lexer::TokenKind::Usize:
    case lexer::TokenKind::F32:
    case lexer::TokenKind::F64:
    case lexer::TokenKind::Bool:
    case lexer::TokenKind::Str: return true;
    default: return false;
  }
}

bool is_prefix_operator(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Minus:
    case lexer::TokenKind::Bang:
    case lexer::TokenKind::Tilde:
    case lexer::TokenKind::Amp:
    case lexer::TokenKind::Star:
    case lexer::TokenKind::StarStar:
    case lexer::TokenKind::Hash: return true;
    default: return false;
  }
}

// Whether a token in front of an expression lets a following prefix
// operator read as a prefix rather than a binary one.
bool allows_prefix_after(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Plus:
    case lexer::TokenKind::Minus:
    case lexer::TokenKind::Star:
    case lexer::TokenKind::Slash:
    case lexer::TokenKind::Percent:
    case lexer::TokenKind::StarStar:
    case lexer::TokenKind::Amp:
    case lexer::TokenKind::Pipe:
    case lexer::TokenKind::Caret:
    case lexer::TokenKind::Tilde:
    case lexer::TokenKind::LessLess:
    case lexer::TokenKind::GreaterGreater:
    case lexer::TokenKind::PlusEq:
    case lexer::TokenKind::MinusEq:
    case lexer::TokenKind::StarEq:
    case lexer::TokenKind::SlashEq:
    case lexer::TokenKind::PercentEq:
    case lexer::TokenKind::StarStarEq:
    case lexer::TokenKind::AmpEq:
    case lexer::TokenKind::PipeEq:
    case lexer::TokenKind::CaretEq:
    case lexer::TokenKind::LessLessEq:
    case lexer::TokenKind::GreaterGreaterEq:
    case lexer::TokenKind::ColonEq:
    case lexer::TokenKind::Eq:
    case lexer::TokenKind::EqEq:
    case lexer::TokenKind::Bang:
    case lexer::TokenKind::AmpAmp:
    case lexer::TokenKind::PipePipe:
    case lexer::TokenKind::BangEq:
    case lexer::TokenKind::Greater:
    case lexer::TokenKind::Less:
    case lexer::TokenKind::GreaterEq:
    case lexer::TokenKind::LessEq:
    case lexer::TokenKind::Arrow:
    case lexer::TokenKind::FatArrow:
    case lexer::TokenKind::Colon:
    case lexer::TokenKind::Comma:
    case lexer::TokenKind::As:
    case lexer::TokenKind::In:
    case lexer::TokenKind::Ret:
    case lexer::TokenKind::If:
    case lexer::TokenKind::Else:
    case lexer::TokenKind::While:
    case lexer::TokenKind::For:
    case lexer::TokenKind::Match:
    case lexer::TokenKind::DotDot:
    case lexer::TokenKind::DotDotEq:
    case lexer::TokenKind::DotDotLess: return true;
    default: return false;
  }
}

// Whether a line may break after this operator. The grammar continues an
// expression at a newline only when the line it leaves cannot end a
// statement, so a break belongs after an operator rather than before one.
bool is_break_after(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Plus:
    case lexer::TokenKind::Minus:
    case lexer::TokenKind::Star:
    case lexer::TokenKind::Slash:
    case lexer::TokenKind::Percent:
    case lexer::TokenKind::StarStar:
    case lexer::TokenKind::Amp:
    case lexer::TokenKind::Pipe:
    case lexer::TokenKind::Caret:
    case lexer::TokenKind::LessLess:
    case lexer::TokenKind::GreaterGreater:
    case lexer::TokenKind::PlusEq:
    case lexer::TokenKind::MinusEq:
    case lexer::TokenKind::StarEq:
    case lexer::TokenKind::SlashEq:
    case lexer::TokenKind::PercentEq:
    case lexer::TokenKind::StarStarEq:
    case lexer::TokenKind::AmpEq:
    case lexer::TokenKind::PipeEq:
    case lexer::TokenKind::CaretEq:
    case lexer::TokenKind::LessLessEq:
    case lexer::TokenKind::GreaterGreaterEq:
    case lexer::TokenKind::ColonEq:
    case lexer::TokenKind::Eq:
    case lexer::TokenKind::EqEq:
    case lexer::TokenKind::AmpAmp:
    case lexer::TokenKind::PipePipe:
    case lexer::TokenKind::BangEq:
    case lexer::TokenKind::Greater:
    case lexer::TokenKind::Less:
    case lexer::TokenKind::GreaterEq:
    case lexer::TokenKind::LessEq:
    case lexer::TokenKind::Arrow:
    case lexer::TokenKind::FatArrow:
    case lexer::TokenKind::As:
    case lexer::TokenKind::In: return true;
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

  Doc run(const cst::Node& root) {
    const std::vector<Chunk> chunks =
        collect_chunks(root, cst::BraceRole::Block);
    if (chunks.empty()) {
      return doc_empty();
    }
    return print_body_lines(chunks, cst::BraceRole::Block, true);
  }

 private:
  enum class Sep : u8 { None, Space, Line, SoftLine };

  struct Piece {
    Doc doc;
    Sep sep = Sep::None;
  };

  struct Item {
    std::vector<const cst::Node*> nodes;
  };

  struct Chunk {
    std::vector<const cst::Node*> nodes;
    bool is_comment = false;
    usize newlines_before = 0;
    // The chunk ended at a semicolon spelled in the source rather than
    // at the newline the lexer turned into one.
    bool explicit_semi = false;
    // Where the whitespace before a comment that rode the line a
    // separator finished starts: after the separator itself, whose
    // bytes the terminator already wrote.
    bool has_riding_gap = false;
    usize riding_gap_start = 0;
  };

  std::vector<Chunk> collect_chunks(const cst::Node& seq,
                                    cst::BraceRole role) const;
  Doc chunk_line(const Chunk& chunk, Doc terminator);
  static usize trailing_eol_start(const Chunk& chunk);
  static bool ends_in_block(const std::vector<const cst::Node*>& code);
  static bool keeps_semicolon(const Chunk& chunk,
                              const std::vector<const cst::Node*>& code,
                              cst::BraceRole role,
                              bool last_chunk,
                              bool next_suppresses);
  static bool chunk_is_semi_terminated_item(const Chunk& chunk);
  static bool chunk_starts_with_suppressor(const Chunk& chunk);
  Doc print_body_lines(const std::vector<Chunk>& chunks,
                       cst::BraceRole role,
                       bool top_level);
  Doc chunk_terminator(const Chunk& chunk,
                       const std::vector<const cst::Node*>& code,
                       cst::BraceRole role,
                       bool last_chunk,
                       bool next_suppresses);
  static bool ends_like_statement(const std::vector<const cst::Node*>& nodes);
  Doc print_line(const std::vector<const cst::Node*>& nodes);
  Doc print_flow(const std::vector<const cst::Node*>& nodes, bool allow_wrap);
  Doc build_flow(std::vector<Piece> pieces, bool allow_wrap);
  Doc node_doc(const cst::Node& node);
  Doc print_group(const cst::Node& group);
  Doc print_brace_body(const cst::Node& group);
  Doc print_list(const cst::Node& group,
                 std::string_view open,
                 std::string_view close);
  Doc print_literal(const cst::Node& group);
  std::vector<Item> split_on_commas(const cst::Node& seq,
                                    bool& had_comma,
                                    bool& trailing) const;
  bool space_before(const cst::Node* prev,
                    bool prev_generic,
                    bool prev_unary,
                    const cst::Node& cur,
                    bool cur_generic) const;
  void compute_generic(const std::vector<const cst::Node*>& nodes,
                       std::vector<bool>& generic) const;
  bool looks_like_generic(const std::vector<const cst::Node*>& nodes,
                          usize i) const;
  static const cst::Node* previous_significant(
      const std::vector<const cst::Node*>& nodes,
      usize i);
  static const cst::Node* next_significant(
      const std::vector<const cst::Node*>& nodes,
      usize i);
  static bool chunk_is_use(const Chunk& chunk);

  std::string_view source_;
  FormatConfig config_;
};

const cst::Node* Printer::previous_significant(
    const std::vector<const cst::Node*>& nodes,
    usize i) {
  while (i > 0) {
    --i;
    if (nodes[i]->kind != cst::NodeKind::Comment) {
      return nodes[i];
    }
  }
  return nullptr;
}

const cst::Node* Printer::next_significant(
    const std::vector<const cst::Node*>& nodes,
    usize i) {
  while (i < nodes.size()) {
    if (nodes[i]->kind != cst::NodeKind::Comment) {
      return nodes[i];
    }
    ++i;
  }
  return nullptr;
}

bool Printer::space_before(const cst::Node* prev,
                           bool prev_generic,
                           bool prev_unary,
                           const cst::Node& cur,
                           bool cur_generic) const {
  if (prev == nullptr) {
    return false;
  }
  if (cur.kind == cst::NodeKind::Token) {
    switch (cur.token) {
      case lexer::TokenKind::Comma:
      case lexer::TokenKind::Semicolon:
      case lexer::TokenKind::Dot:
      case lexer::TokenKind::ColonColon:
      case lexer::TokenKind::Question:
      case lexer::TokenKind::Colon:
      case lexer::TokenKind::RParen:
      case lexer::TokenKind::RBracket:
      case lexer::TokenKind::RBrace: return false;
      // A range reads tight against what bounds it, except in the one
      // place the language spells it loose: a parameter whose type is
      // the variadic run of dots.
      case lexer::TokenKind::DotDot:
      case lexer::TokenKind::DotDotEq:
      case lexer::TokenKind::DotDotLess:
        return prev->kind == cst::NodeKind::Token &&
               prev->token == lexer::TokenKind::Colon;
      case lexer::TokenKind::Less:
      case lexer::TokenKind::Greater:
      case lexer::TokenKind::GreaterGreater:
        if (cur_generic) {
          return false;
        }
        break;
      default: break;
    }
  }
  if (cur.kind == cst::NodeKind::Group &&
      (cur.group_role == cst::GroupRole::Paren ||
       cur.group_role == cst::GroupRole::Bracket)) {
    if (prev->kind == cst::NodeKind::Group &&
        (prev->group_role == cst::GroupRole::Paren ||
         prev->group_role == cst::GroupRole::Bracket)) {
      return false;
    }
    if (prev->kind == cst::NodeKind::Token) {
      if (is_path_like(prev->token)) {
        return false;
      }
      if ((prev->token == lexer::TokenKind::Greater ||
           prev->token == lexer::TokenKind::GreaterGreater) &&
          prev_generic) {
        return false;
      }
      if (prev->token == lexer::TokenKind::ColonColon) {
        return false;
      }
    }
  }
  if (prev->kind == cst::NodeKind::Token) {
    switch (prev->token) {
      case lexer::TokenKind::Dot:
      case lexer::TokenKind::ColonColon:
      case lexer::TokenKind::Question:
      case lexer::TokenKind::DotDot:
      case lexer::TokenKind::DotDotEq:
      case lexer::TokenKind::DotDotLess: return false;
      // A generic list's opening bracket touches its first argument;
      // its closing bracket touches the path it finishes -- the call or
      // the index it opens, and the member it names. A brace is a
      // header or a literal, and an identifier is an impl's trait, and
      // both read with the space.
      case lexer::TokenKind::Less:
        if (prev_generic) {
          return false;
        }
        break;
      case lexer::TokenKind::Greater:
      case lexer::TokenKind::GreaterGreater:
        if (prev_generic && ((cur.kind == cst::NodeKind::Group &&
                              cur.group_role != cst::GroupRole::Brace) ||
                             (cur.kind == cst::NodeKind::Token &&
                              cur.token == lexer::TokenKind::ColonColon))) {
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

void Printer::compute_generic(const std::vector<const cst::Node*>& nodes,
                              std::vector<bool>& generic) const {
  usize depth = 0;
  for (usize i = 0; i < nodes.size(); ++i) {
    const cst::Node& node = *nodes[i];
    if (node.kind != cst::NodeKind::Token) {
      continue;
    }
    if (node.token == lexer::TokenKind::Less) {
      if (looks_like_generic(nodes, i)) {
        generic[i] = true;
        ++depth;
      }
    } else if (node.token == lexer::TokenKind::Greater) {
      if (depth > 0) {
        generic[i] = true;
        --depth;
      }
    } else if (node.token == lexer::TokenKind::GreaterGreater) {
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

bool Printer::looks_like_generic(const std::vector<const cst::Node*>& nodes,
                                 usize i) const {
  const cst::Node* prev = previous_significant(nodes, i);
  if (prev == nullptr || prev->kind != cst::NodeKind::Token) {
    return false;
  }
  const bool impl_params = prev->token == lexer::TokenKind::Impl;
  const bool prev_ok = is_path_like(prev->token) ||
                       prev->token == lexer::TokenKind::ColonColon ||
                       prev->token == lexer::TokenKind::Greater ||
                       prev->token == lexer::TokenKind::GreaterGreater ||
                       impl_params;
  if (!prev_ok) {
    return false;
  }
  usize depth = 1;
  usize j = i + 1;
  for (; j < nodes.size(); ++j) {
    const cst::Node& node = *nodes[j];
    if (node.kind == cst::NodeKind::Comment) {
      continue;
    }
    if (node.kind == cst::NodeKind::Group) {
      continue;
    }
    switch (node.token) {
      case lexer::TokenKind::Less: ++depth; break;
      case lexer::TokenKind::Greater:
        if (depth > 0) {
          --depth;
        }
        break;
      case lexer::TokenKind::GreaterGreater:
        depth = depth >= 2 ? depth - 2 : 0;
        break;
      case lexer::TokenKind::Semicolon:
      case lexer::TokenKind::Eq:
      case lexer::TokenKind::ColonEq:
      case lexer::TokenKind::AmpAmp:
      case lexer::TokenKind::PipePipe:
      case lexer::TokenKind::If:
      case lexer::TokenKind::Else:
      case lexer::TokenKind::Match:
      case lexer::TokenKind::While:
      case lexer::TokenKind::For:
      case lexer::TokenKind::Loop:
      case lexer::TokenKind::Fn:
      case lexer::TokenKind::Ret:
      case lexer::TokenKind::Struct:
      case lexer::TokenKind::Enum:
      case lexer::TokenKind::Impl:
      case lexer::TokenKind::Spec: return false;
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
  const cst::Node* follower = next_significant(nodes, j);
  if (follower == nullptr) {
    return true;
  }
  if (follower->kind == cst::NodeKind::Group) {
    return true;
  }
  if (impl_params && follower->token == lexer::TokenKind::Ident) {
    return true;
  }
  switch (follower->token) {
    case lexer::TokenKind::Comma:
    case lexer::TokenKind::RParen:
    case lexer::TokenKind::RBracket:
    case lexer::TokenKind::RBrace:
    case lexer::TokenKind::ColonColon:
    case lexer::TokenKind::Dot:
    case lexer::TokenKind::Question:
    case lexer::TokenKind::Semicolon:
    case lexer::TokenKind::Greater:
    case lexer::TokenKind::GreaterGreater:
    case lexer::TokenKind::Arrow:
    case lexer::TokenKind::FatArrow:
    case lexer::TokenKind::As:
    case lexer::TokenKind::Eq:
    case lexer::TokenKind::ColonEq:
    case lexer::TokenKind::Plus:
    case lexer::TokenKind::Minus:
    case lexer::TokenKind::Star:
    case lexer::TokenKind::Slash:
    case lexer::TokenKind::Percent:
    case lexer::TokenKind::StarStar:
    case lexer::TokenKind::Amp:
    case lexer::TokenKind::Pipe:
    case lexer::TokenKind::Caret:
    case lexer::TokenKind::LessLess:
    case lexer::TokenKind::EqEq:
    case lexer::TokenKind::BangEq:
    case lexer::TokenKind::GreaterEq:
    case lexer::TokenKind::LessEq:
    case lexer::TokenKind::AmpAmp:
    case lexer::TokenKind::PipePipe:
    case lexer::TokenKind::PlusEq:
    case lexer::TokenKind::MinusEq:
    case lexer::TokenKind::StarEq:
    case lexer::TokenKind::SlashEq:
    case lexer::TokenKind::PercentEq:
    case lexer::TokenKind::StarStarEq:
    case lexer::TokenKind::AmpEq:
    case lexer::TokenKind::PipeEq:
    case lexer::TokenKind::CaretEq:
    case lexer::TokenKind::LessLessEq:
    case lexer::TokenKind::GreaterGreaterEq: return true;
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

Doc Printer::print_flow(const std::vector<const cst::Node*>& nodes,
                        bool allow_wrap) {
  std::vector<bool> generic(nodes.size(), false);
  compute_generic(nodes, generic);
  std::vector<bool> unary(nodes.size(), false);
  for (usize i = 0; i < nodes.size(); ++i) {
    const cst::Node& node = *nodes[i];
    if (node.kind == cst::NodeKind::Token && is_prefix_operator(node.token)) {
      const cst::Node* prev = previous_significant(nodes, i);
      unary[i] = prev == nullptr || (prev->kind == cst::NodeKind::Token &&
                                     allows_prefix_after(prev->token));
    }
  }

  std::vector<Piece> pieces;
  const cst::Node* prev = nullptr;
  // The node immediately before this one, whatever it is: the gap the
  // source put between two nodes is what an error token's neighbours
  // are pinned against.
  const cst::Node* prev_node = nullptr;
  bool prev_generic = false;
  bool prev_unary = false;
  bool pending_break = false;
  bool suppress = false;

  const auto is_error_token = [](const cst::Node* node) {
    return node != nullptr && node->kind == cst::NodeKind::Token &&
           node->token == lexer::TokenKind::Error;
  };
  // The whitespace the source put around an error token, as its own
  // piece. An error's bytes re-lex as whatever the bytes around them
  // make of them -- a lone quote becomes a literal when a space opens
  // room for its closing half -- so the gap is pinned verbatim: it is
  // the one thing that keeps a second pass reading the same stream.
  const auto pin_gap = [&](const cst::Node& from, const cst::Node& to) {
    usize gap_start = from.offset + from.length;
    if (from.kind == cst::NodeKind::Token &&
        from.token == lexer::TokenKind::Error) {
      // The blanks an error span ends in are gap bytes, not span
      // bytes: the span re-absorbs one of them on every pass, so they
      // all ride in the gap and the count stays put.
      gap_start = from.offset + error_text(source_, from).size();
    }
    if (to.offset > gap_start) {
      pieces.push_back(
          Piece{doc_text(source_.substr(gap_start, to.offset - gap_start)),
                Sep::None});
    }
  };

  for (usize i = 0; i < nodes.size(); ++i) {
    const cst::Node& node = *nodes[i];
    if (node.kind == cst::NodeKind::Comment) {
      Sep sep = Sep::None;
      if (!suppress && !pieces.empty()) {
        sep = Sep::Space;
      }
      if (sep == Sep::Space && is_error_token(prev_node)) {
        pin_gap(*prev_node, node);
        sep = Sep::None;
      }
      Doc text = doc_text(node_text(source_, node));
      // A comment that runs to end of line ends the line it sits on, so
      // what follows it has to start a new one. A block comment does
      // not: code may keep reading after it on the same line.
      const bool runs_to_eol = node.comment != cst::CommentKind::Block;
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
      prev_node = &node;
      continue;
    }
    Sep sep = Sep::None;
    const bool near_error = node.kind == cst::NodeKind::Token &&
                            node.token == lexer::TokenKind::Error;
    const bool after_comment_break = suppress;
    if (pieces.empty()) {
      sep = Sep::None;
    } else if (suppress) {
      sep = Sep::None;
      suppress = false;
    } else if (near_error || is_error_token(prev_node)) {
      if (prev_node != nullptr) {
        pin_gap(*prev_node, node);
      }
      sep = Sep::None;
    } else if (pending_break) {
      sep = allow_wrap ? Sep::Line : Sep::Space;
    } else if (node.kind == cst::NodeKind::Token &&
               node.token == lexer::TokenKind::Dot && prev != nullptr &&
               prev->kind == cst::NodeKind::Token &&
               (prev->token == lexer::TokenKind::Integer ||
                prev->token == lexer::TokenKind::Float) &&
               dot_joins_number(i + 1 < nodes.size()
                                    ? node_text(source_, *nodes[i + 1])
                                    : std::string_view())) {
      // A dot after a number reads as the number's decimal point when
      // the two touch and the byte past the dot continues a number: a
      // member name does not, a digit and the float suffixes do. The
      // token count of the output would differ from the tree's, so
      // the two never touch there.
      sep = Sep::Space;
    } else if (node.kind == cst::NodeKind::Token &&
               node.token == lexer::TokenKind::Dot &&
               !(prev != nullptr && prev->kind == cst::NodeKind::Token &&
                 (prev->token == lexer::TokenKind::Dot ||
                  prev->token == lexer::TokenKind::DotDot))) {
      // The chain continues on the next line at a member boundary. In
      // the middle of a run of dots it must not: a break there
      // re-pairs the dots into range tokens on the next pass, and the
      // layout moves with the pairing.
      sep = allow_wrap ? Sep::SoftLine : Sep::None;
    } else if (space_before(prev, prev_generic, prev_unary, node, generic[i])) {
      sep = Sep::Space;
    }
    // Nothing between two tokens can read back as one token, whatever
    // the spacing rules wanted: the merge is checked after them, over
    // the seam the layout actually wrote.
    if (sep == Sep::None && !after_comment_break && !near_error &&
        !is_error_token(prev_node) && prev_node != nullptr &&
        prev_node->kind == cst::NodeKind::Token &&
        node.kind == cst::NodeKind::Token &&
        tokens_merge(node_text(source_, *prev_node),
                     node_text(source_, node))) {
      sep = Sep::Space;
    }
    pieces.push_back(Piece{node_doc(node), sep});
    // A generic bracket reads as a delimiter here, not as a comparison:
    // a break inside an argument list would space it flat.
    pending_break = node.kind == cst::NodeKind::Token && !unary[i] &&
                    !generic[i] && is_break_after(node.token);
    prev = &node;
    prev_node = &node;
    prev_generic = generic[i];
    prev_unary = unary[i];
  }
  return build_flow(std::move(pieces), allow_wrap);
}

Doc Printer::node_doc(const cst::Node& node) {
  switch (node.kind) {
    case cst::NodeKind::Token:
      if (node.token == lexer::TokenKind::Error) {
        return doc_text(error_text(source_, node));
      }
      return doc_text(node_text(source_, node));
    case cst::NodeKind::Comment: return doc_text(node_text(source_, node));
    case cst::NodeKind::Group: return print_group(node);
    case cst::NodeKind::Sequence: return doc_empty();
  }
  return doc_empty();
}

Doc Printer::print_group(const cst::Node& group) {
  switch (group.group_role) {
    case cst::GroupRole::Paren: return print_list(group, "(", ")");
    case cst::GroupRole::Bracket: return print_list(group, "[", "]");
    case cst::GroupRole::Brace:
      if (group.brace_role == cst::BraceRole::Literal) {
        return print_literal(group);
      }
      return print_brace_body(group);
  }
  return doc_empty();
}

Doc Printer::print_brace_body(const cst::Node& group) {
  const std::string close = group.has_closer ? "}" : "";
  if (group.kids.empty()) {
    std::vector<Doc> parts;
    parts.push_back(doc_text("{"));
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }
  const cst::Node& inner = group.kids[0];
  const cst::BraceRole role = group.brace_role;
  const std::vector<Chunk> chunks = collect_chunks(inner, role);

  const bool member_body = role == cst::BraceRole::ImplBody ||
                           role == cst::BraceRole::SpecBody ||
                           role == cst::BraceRole::ExternBody;
  if (chunks.empty()) {
    // A member body still opens its lines: an impl that owns nothing
    // reads as the pair of braces the grammar gives it.
    std::vector<Doc> parts;
    parts.push_back(doc_text("{"));
    if (member_body) {
      parts.push_back(doc_hardline());
    }
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }
  // An expression block holds one expression, and reads like the value
  // it wraps: on the line it started while it fits, over lines when it
  // does not. A comment that ends its line keeps it off the one-line
  // form, since the closer would land inside the comment.
  if (role == cst::BraceRole::ExprBlock && chunks.size() == 1 &&
      !chunks[0].is_comment &&
      trailing_eol_start(chunks[0]) == chunks[0].nodes.size() &&
      chunks[0].newlines_before < 2) {
    const usize end = trailing_eol_start(chunks[0]);
    std::vector<const cst::Node*> code(
        chunks[0].nodes.begin(),
        chunks[0].nodes.begin() + static_cast<std::ptrdiff_t>(end));
    std::vector<Doc> inner_parts;
    inner_parts.push_back(doc_line());
    inner_parts.push_back(chunk_line(
        chunks[0], chunk_terminator(chunks[0], code, role, true, false)));
    std::vector<Doc> parts;
    parts.push_back(doc_text("{"));
    parts.push_back(
        doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
    parts.push_back(doc_line());
    parts.push_back(doc_text(close));
    return doc_group(doc_concat(std::move(parts)));
  }
  Doc body = print_body_lines(chunks, role, false);
  std::vector<Doc> parts;
  parts.push_back(doc_text("{"));
  std::vector<Doc> inner_parts;
  inner_parts.push_back(doc_hardline());
  inner_parts.push_back(std::move(body));
  parts.push_back(
      doc_nest(config_.indent_width, doc_concat(std::move(inner_parts))));
  parts.push_back(doc_hardline());
  parts.push_back(doc_text(close));
  return doc_concat(std::move(parts));
}

std::vector<Printer::Item> Printer::split_on_commas(const cst::Node& seq,
                                                    bool& had_comma,
                                                    bool& trailing) const {
  std::vector<Item> items;
  Item current;
  had_comma = false;
  trailing = false;
  for (const cst::Node& node : seq.kids) {
    if (node.kind == cst::NodeKind::Token &&
        node.token == lexer::TokenKind::Comma) {
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

Doc Printer::print_list(const cst::Node& group,
                        std::string_view open,
                        std::string_view close) {
  if (group.kids.empty()) {
    std::vector<Doc> parts;
    parts.push_back(doc_text(open));
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
  }
  const cst::Node& inner = group.kids[0];
  bool had_comma = false;
  bool trailing = false;
  std::vector<Item> items = split_on_commas(inner, had_comma, trailing);
  if (!group.has_closer) {
    std::vector<const cst::Node*> nodes;
    nodes.reserve(inner.kids.size());
    for (const cst::Node& node : inner.kids) {
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
  // A comma the source did not write lands inside a delimiter it
  // never closed and grows by one per pass, so an unclosed list
  // invents none; the comma the source did write is its own byte and
  // prints whatever the group's shape.
  if (trailing || (group.has_closer && items.size() >= 2)) {
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

Doc Printer::print_literal(const cst::Node& group) {
  if (group.kids.empty()) {
    return doc_text("{}");
  }
  const cst::Node& inner = group.kids[0];
  bool had_comma = false;
  bool trailing = false;
  std::vector<Item> items = split_on_commas(inner, had_comma, trailing);
  const std::string close = group.has_closer ? "}" : "";
  if (items.empty() || (items.size() == 1 && items[0].nodes.empty())) {
    // A closer the source never wrote is not invented here either:
    // the next pass would read it as the end of a group this one left
    // open.
    std::vector<Doc> parts;
    parts.push_back(doc_text("{"));
    if (had_comma) {
      parts.push_back(doc_text(","));
    }
    parts.push_back(doc_text(close));
    return doc_concat(std::move(parts));
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
  // An unclosed literal grows an invented comma by one per pass, the
  // same way an unclosed list does; the comma the source wrote is its
  // own byte and stays.
  if (trailing || (group.has_closer && items.size() >= 2)) {
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

std::vector<Printer::Chunk> Printer::collect_chunks(const cst::Node& seq,
                                                    cst::BraceRole role) const {
  const bool split_comma = role == cst::BraceRole::StructBody ||
                           role == cst::BraceRole::EnumBody ||
                           role == cst::BraceRole::MatchBody;
  std::vector<Chunk> chunks;
  Chunk current;
  // The previous lexeme ended a chunk: a comment on the same line as
  // the separator rides the line that separator finished rather than
  // starting one of its own, or the layout would move it on the pass
  // over this layout's own output.
  bool after_separator = false;
  const auto flush = [&]() {
    if (!current.nodes.empty()) {
      chunks.push_back(std::move(current));
      current = Chunk{};
    }
  };
  usize separator_end = 0;
  for (const cst::Node& node : seq.kids) {
    if (node.kind == cst::NodeKind::Token &&
        node.token == lexer::TokenKind::Semicolon) {
      // A semicolon with no chunk in front of it ends nothing: the
      // grammar skips a stray one, and its flag must not leak into the
      // chunk that follows.
      if (!current.nodes.empty()) {
        current.explicit_semi = !node.synthetic_semi;
      }
      flush();
      after_separator = true;
      separator_end = node.offset + node.length;
      continue;
    }
    if (split_comma && node.kind == cst::NodeKind::Token &&
        node.token == lexer::TokenKind::Comma) {
      flush();
      after_separator = true;
      separator_end = node.offset + node.length;
      continue;
    }
    if (node.kind == cst::NodeKind::Comment) {
      const bool same_line = node.newlines_before == 0 &&
                             (!current.nodes.empty() || after_separator);
      if (same_line) {
        if (current.nodes.empty() && !chunks.empty()) {
          chunks.back().nodes.push_back(&node);
          chunks.back().has_riding_gap = true;
          chunks.back().riding_gap_start = separator_end;
        } else {
          current.nodes.push_back(&node);
        }
        if (node.comment == cst::CommentKind::Line) {
          flush();
        }
        after_separator = false;
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
    after_separator = false;
  }
  flush();
  return chunks;
}

// Whether a run of nodes is one the lexer would end a statement after, so
// the newline that replaces a spelled-out semicolon reads back as one.
bool Printer::ends_like_statement(const std::vector<const cst::Node*>& nodes) {
  for (usize i = nodes.size(); i > 0; --i) {
    const cst::Node& node = *nodes[i - 1];
    if (node.kind == cst::NodeKind::Comment) {
      continue;
    }
    if (node.kind == cst::NodeKind::Group) {
      return node.has_closer;
    }
    switch (node.token) {
      case lexer::TokenKind::Ident:
      case lexer::TokenKind::Underscore:
      case lexer::TokenKind::Integer:
      case lexer::TokenKind::Float:
      case lexer::TokenKind::String:
      case lexer::TokenKind::Char:
      case lexer::TokenKind::True:
      case lexer::TokenKind::False:
      case lexer::TokenKind::Break:
      case lexer::TokenKind::Continue:
      case lexer::TokenKind::Ret:
      case lexer::TokenKind::Question:
      case lexer::TokenKind::DotDot:
      case lexer::TokenKind::DotDotEq:
      case lexer::TokenKind::DotDotLess:
      case lexer::TokenKind::Self:
      case lexer::TokenKind::SelfType:
      case lexer::TokenKind::I8:
      case lexer::TokenKind::I16:
      case lexer::TokenKind::I32:
      case lexer::TokenKind::I64:
      case lexer::TokenKind::I128:
      case lexer::TokenKind::Isize:
      case lexer::TokenKind::U8:
      case lexer::TokenKind::U16:
      case lexer::TokenKind::U32:
      case lexer::TokenKind::U64:
      case lexer::TokenKind::U128:
      case lexer::TokenKind::Usize:
      case lexer::TokenKind::F32:
      case lexer::TokenKind::F64:
      case lexer::TokenKind::Bool:
      case lexer::TokenKind::Str: return true;
      default: return false;
    }
  }
  return false;
}

bool Printer::chunk_is_use(const Chunk& chunk) {
  for (usize i = 0; i < chunk.nodes.size(); ++i) {
    const cst::Node& node = *chunk.nodes[i];
    if (node.kind != cst::NodeKind::Token) {
      continue;
    }
    if (node.token == lexer::TokenKind::Use) {
      return true;
    }
    if (node.token == lexer::TokenKind::Pub) {
      for (usize j = i + 1; j < chunk.nodes.size(); ++j) {
        if (chunk.nodes[j]->kind == cst::NodeKind::Token &&
            chunk.nodes[j]->token == lexer::TokenKind::Use) {
          return true;
        }
      }
      return false;
    }
    return false;
  }
  return false;
}

// Where the chunk's run of trailing comments that end a line starts.
// A block comment does not end a line, so it stays part of the code.
usize Printer::trailing_eol_start(const Chunk& chunk) {
  usize end = chunk.nodes.size();
  while (end > 0) {
    const cst::Node& node = *chunk.nodes[end - 1];
    if (node.kind != cst::NodeKind::Comment ||
        node.comment == cst::CommentKind::Block) {
      break;
    }
    --end;
  }
  return end;
}

// Whether the code ends in a statement block rather than an
// expression: a match arm whose body is a block takes no separator
// after it, while one whose body is a match or a struct literal is an
// expression arm and keeps its comma.
bool Printer::ends_in_block(const std::vector<const cst::Node*>& code) {
  for (usize i = code.size(); i > 0; --i) {
    const cst::Node* node = code[i - 1];
    if (node->kind == cst::NodeKind::Comment) {
      continue;
    }
    if (node->kind != cst::NodeKind::Group ||
        node->group_role != cst::GroupRole::Brace) {
      return false;
    }
    return node->brace_role == cst::BraceRole::Block ||
           node->brace_role == cst::BraceRole::ExprBlock ||
           node->brace_role == cst::BraceRole::FnBody;
  }
  return false;
}

// A spelled-out semicolon survives wherever the newline that replaces
// it would not read the same. A spec or extern member is terminated by
// one outright. The last chunk of a body keeps its statement status:
// without the semicolon the run becomes the block's trailing
// expression instead of a statement whose value nobody takes. A run the
// lexer does not end a statement after (an intrinsic signature that
// closes in a greater-than) keeps it too. Everywhere else the line
// break reads the same to the parser.
// Whether the chunk is an item the grammar terminates with a
// semicolon: a use, or an intrinsic signature. The newline a formatter
// writes reads the same to the compiler, but the grammar spells these
// terminators, so the layout does too.
bool Printer::chunk_is_semi_terminated_item(const Chunk& chunk) {
  for (const cst::Node* node : chunk.nodes) {
    if (node->kind == cst::NodeKind::Token &&
        (node->token == lexer::TokenKind::Use ||
         node->token == lexer::TokenKind::Intrinsic)) {
      return true;
    }
  }
  return false;
}

// Whether a chunk begins with a token the lexer's semicolon-insertion
// rule suppresses: a line break in front of one of these does not
// stand in for a semicolon, so the spelled one ahead of it cannot be
// dropped without merging the two chunks into one.
bool Printer::chunk_starts_with_suppressor(const Chunk& chunk) {
  for (const cst::Node* node : chunk.nodes) {
    if (node->kind != cst::NodeKind::Token) {
      return false;
    }
    switch (node->token) {
      case lexer::TokenKind::Comma:
      case lexer::TokenKind::RParen:
      case lexer::TokenKind::RBracket:
      case lexer::TokenKind::RBrace:
      case lexer::TokenKind::Dot:
      // The whole dot family: the lexer's probe reads the next
      // character, and every token that starts with a dot reads as a
      // chain continuation to it, so a line break before any of them
      // stands without a semicolon.
      case lexer::TokenKind::DotDot:
      case lexer::TokenKind::DotDotEq:
      case lexer::TokenKind::DotDotLess:
      case lexer::TokenKind::Else: return true;
      default: return false;
    }
  }
  return false;
}

bool Printer::keeps_semicolon(const Chunk& chunk,
                              const std::vector<const cst::Node*>& code,
                              cst::BraceRole role,
                              bool last_chunk,
                              bool next_suppresses) {
  // The items the grammar terminates with a semicolon carry it whether
  // the source spelled it or left it to the newline: a reader without
  // the insertion rule, the tree-sitter grammar among them, has to see
  // the terminator the specification writes.
  if (chunk_is_semi_terminated_item(chunk)) {
    return true;
  }
  if (!chunk.explicit_semi) {
    return false;
  }
  if (role == cst::BraceRole::SpecBody || role == cst::BraceRole::ExternBody) {
    return true;
  }
  if (next_suppresses) {
    return true;
  }
  if (last_chunk) {
    return true;
  }
  return !ends_like_statement(code);
}

// The line one chunk contributes: its code, the separator that ends it,
// and the comments that ride after it. A comment chunk is its own text.
Doc Printer::chunk_line(const Chunk& chunk, Doc terminator) {
  if (chunk.is_comment) {
    return doc_text(node_text(source_, *chunk.nodes.front()));
  }
  const usize end = trailing_eol_start(chunk);
  std::vector<const cst::Node*> code(
      chunk.nodes.begin(),
      chunk.nodes.begin() + static_cast<std::ptrdiff_t>(end));
  std::vector<Doc> parts;
  parts.push_back(print_line(code));
  if (terminator.kind != DocKind::Empty) {
    parts.push_back(std::move(terminator));
  }
  // The space rides only after code: a chunk that is nothing but its
  // comments starts the line with them, and a pass over a layout that
  // began each such line with a space would read the space back and
  // move the comment again. After an error the source's own gap is
  // pinned instead of the canonical space -- those bytes decide what
  // the error's bytes read back as, and a comment that moved would
  // feed them to the span.
  for (usize i = end; i < chunk.nodes.size(); ++i) {
    if (end > 0) {
      const cst::Node& last_code = *chunk.nodes[end - 1];
      const cst::Node& comment = *chunk.nodes[i];
      const bool rides_separator =
          i == end && chunk.has_riding_gap && chunk.riding_gap_start > 0;
      if (last_code.kind == cst::NodeKind::Token &&
          last_code.token == lexer::TokenKind::Error) {
        const usize gap_start =
            rides_separator
                ? chunk.riding_gap_start
                : last_code.offset + error_text(source_, last_code).size();
        if (comment.offset > gap_start) {
          parts.push_back(
              doc_text(source_.substr(gap_start, comment.offset - gap_start)));
        }
      } else if (rides_separator && comment.offset > chunk.riding_gap_start) {
        parts.push_back(doc_text(source_.substr(
            chunk.riding_gap_start, comment.offset - chunk.riding_gap_start)));
      } else {
        parts.push_back(doc_text(" "));
      }
    }
    parts.push_back(doc_text(node_text(source_, *chunk.nodes[i])));
  }
  return doc_concat(std::move(parts));
}

// One chunk per line, with the blank lines the source had between them.
Doc Printer::print_body_lines(const std::vector<Chunk>& chunks,
                              cst::BraceRole role,
                              bool top_level) {
  const bool member = top_level || role == cst::BraceRole::ImplBody ||
                      role == cst::BraceRole::SpecBody ||
                      role == cst::BraceRole::ExternBody;
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
    const usize end = trailing_eol_start(chunks[k]);
    std::vector<const cst::Node*> code(
        chunks[k].nodes.begin(),
        chunks[k].nodes.begin() + static_cast<std::ptrdiff_t>(end));
    bool next_suppresses = false;
    for (usize j = k + 1; j < chunks.size(); ++j) {
      if (chunks[j].is_comment) {
        continue;
      }
      next_suppresses = chunk_starts_with_suppressor(chunks[j]);
      break;
    }
    parts.push_back(chunk_line(
        chunks[k], chunk_terminator(chunks[k], code, role,
                                    k + 1 == chunks.size(), next_suppresses)));
  }
  return doc_concat(std::move(parts));
}

// The separator that ends one chunk of a body. A field and a variant
// are comma-terminated; a match arm takes a comma unless its body is a
// block, which the line break already separates, and one the source
// spelled with a semicolon keeps it; everywhere else only a
// load-bearing semicolon appears.
Doc Printer::chunk_terminator(const Chunk& chunk,
                              const std::vector<const cst::Node*>& code,
                              cst::BraceRole role,
                              bool last_chunk,
                              bool next_suppresses) {
  for (usize i = code.size(); i > 0; --i) {
    const cst::Node* node = code[i - 1];
    if (node->kind == cst::NodeKind::Comment) {
      continue;
    }
    // A chunk that ends in a delimiter the source never closed eats
    // whatever follows it in the output, so the next pass would read a
    // terminator as part of the group's contents. No terminator is
    // written after one.
    if (node->kind == cst::NodeKind::Group && !node->has_closer) {
      return doc_empty();
    }
    // A hungry error eats a separator the same way; a complete one
    // ended where it ended, and the separator after it reads back as a
    // separator.
    if (node->kind == cst::NodeKind::Token &&
        node->token == lexer::TokenKind::Error &&
        error_absorbs(error_text(source_, *node))) {
      return doc_empty();
    }
    break;
  }
  if (role == cst::BraceRole::StructBody || role == cst::BraceRole::EnumBody) {
    return doc_text(",");
  }
  if (role == cst::BraceRole::MatchBody) {
    if (chunk.explicit_semi) {
      return doc_text(";");
    }
    if (ends_in_block(code)) {
      return doc_empty();
    }
    return doc_text(",");
  }
  return keeps_semicolon(chunk, code, role, last_chunk, next_suppresses)
             ? doc_text(";")
             : doc_empty();
}

Doc Printer::print_line(const std::vector<const cst::Node*>& nodes) {
  bool has_block = false;
  for (const cst::Node* node : nodes) {
    if (node->kind == cst::NodeKind::Group &&
        node->group_role == cst::GroupRole::Brace &&
        node->brace_role != cst::BraceRole::Literal) {
      has_block = true;
      break;
    }
  }
  return print_flow(nodes, !has_block);
}

}  // namespace

std::string format(std::string_view source, const FormatConfig& config) {
  const std::vector<cst::Lexeme> lexemes = cst::lex(source);
  const cst::Node root = cst::build_cst(lexemes);
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
