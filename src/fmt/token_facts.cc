// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/token_facts.h"

#include <string>
#include <string_view>
#include <vector>

#include "cst/cst.h"
#include "cst/lex.h"
#include "lexer/token.h"

namespace alcy_fmt::token_facts {

using lexer::TokenKind;

std::string_view node_text(std::string_view source, const cst::Node& node) {
  return cst::text_of(source, node.offset, node.length);
}

// The bytes of an error token, printed as they were read. A span that
// reaches the end of the source drops trailing newlines: the lexer's
// invalid-scalar clipping absorbs the line break there -- the one the
// formatter writes itself after the last token -- and a span that grew
// by it in one pass and not the next would move the layout with it.
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
  // Trailing newlines the source followed with another newline are the
  // line's own breaks, which the layout writes itself: a span that kept
  // them reads back having swallowed the layout's break, and the text
  // grows by a line on every pass. A newline the source followed with
  // code stays, because the span ends where the lexer stopped it and
  // the bytes past the break name the next token.
  if (!text.empty() && text.back() == '\n' &&
      (span_end >= source.size() || source[span_end] == '\n')) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.remove_suffix(1);
    }
  }
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
// the file. A broken scalar takes its continuation bytes and stops at
// the first byte that is neither one nor whitespace, so a separator
// after one reads back as a separator.
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
    case lexer::TokenKind::Mut:
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

const cst::Node* previous_significant(
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

const cst::Node* next_significant(const std::vector<const cst::Node*>& nodes,
                                  usize i) {
  while (i < nodes.size()) {
    if (nodes[i]->kind != cst::NodeKind::Comment) {
      return nodes[i];
    }
    ++i;
  }
  return nullptr;
}

bool space_before(const cst::Node* prev,
                  bool prev_generic,
                  bool prev_unary,
                  const cst::Node& cur,
                  bool cur_generic) {
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

void compute_generic(const std::vector<const cst::Node*>& nodes,
                     std::vector<u8>& flags) {
  usize depth = 0;
  for (usize i = 0; i < nodes.size(); ++i) {
    const cst::Node& node = *nodes[i];
    if (node.kind != cst::NodeKind::Token) {
      continue;
    }
    if (node.token == lexer::TokenKind::Less) {
      if (looks_like_generic(nodes, i)) {
        flags[i] |= NODE_GENERIC;
        ++depth;
      }
    } else if (node.token == lexer::TokenKind::Greater) {
      if (depth > 0) {
        flags[i] |= NODE_GENERIC;
        --depth;
      }
    } else if (node.token == lexer::TokenKind::GreaterGreater) {
      if (depth >= 2) {
        flags[i] |= NODE_GENERIC;
        depth -= 2;
      } else if (depth == 1) {
        flags[i] |= NODE_GENERIC;
        depth = 0;
      }
    }
  }
}

bool looks_like_generic(const std::vector<const cst::Node*>& nodes, usize i) {
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

}  // namespace alcy_fmt::token_facts

