// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/src/cst.h"

#include <utility>
#include <vector>

#include "fmt/src/lex.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"

namespace alcy_fmt {

namespace {

// The same budget the compiler applies to nested syntax. Beyond it the
// builder stops descending and treats the opener as a leaf, so a
// pathologically nested input cannot exhaust the stack.
constexpr u32 MAX_DEPTH = 256;

bool is_opener(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::LParen ||
         kind == lexer::TokenKind::LBracket ||
         kind == lexer::TokenKind::LBrace;
}

bool is_closer(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::RBracket ||
         kind == lexer::TokenKind::RBrace;
}

lexer::TokenKind matching_closer(lexer::TokenKind opener) {
  switch (opener) {
    case lexer::TokenKind::LParen: return lexer::TokenKind::RParen;
    case lexer::TokenKind::LBracket: return lexer::TokenKind::RBracket;
    case lexer::TokenKind::LBrace: return lexer::TokenKind::RBrace;
    default: return lexer::TokenKind::Eof;
  }
}

GroupRole group_role_of(lexer::TokenKind opener) {
  switch (opener) {
    case lexer::TokenKind::LParen: return GroupRole::Paren;
    case lexer::TokenKind::LBracket: return GroupRole::Bracket;
    default: return GroupRole::Brace;
  }
}

lexer::TokenKind closer_of_group(const Node& group) {
  switch (group.group_role) {
    case GroupRole::Paren: return lexer::TokenKind::RParen;
    case GroupRole::Bracket: return lexer::TokenKind::RBracket;
    case GroupRole::Brace: return lexer::TokenKind::RBrace;
  }
  return lexer::TokenKind::Eof;
}

// Whether a token ends the header that a following brace belongs to.
bool forces_block_brace(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Arrow:
    case lexer::TokenKind::FatArrow:
    case lexer::TokenKind::Eq:
    case lexer::TokenKind::ColonEq:
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
    case lexer::TokenKind::Ret:
    case lexer::TokenKind::As:
    case lexer::TokenKind::In:
    case lexer::TokenKind::Comma:
    case lexer::TokenKind::Colon:
      return true;
    default: return false;
  }
}

// Reads the role of a brace from the header in front of it. The header is
// the run of siblings since the last separator; the run is flat, because
// every nested delimiter is already a node of its own.
BraceRole classify_brace(const std::vector<Node>& lead) {
  usize start = 0;
  for (usize i = lead.size(); i > 0; --i) {
    const Node& node = lead[i - 1];
    if (node.kind == NodeKind::Token &&
        node.token == lexer::TokenKind::Semicolon) {
      start = i;
      break;
    }
  }

  bool has_match = false;
  bool has_struct = false;
  bool has_enum = false;
  bool has_impl = false;
  bool has_spec = false;
  bool has_extern = false;
  bool has_fn = false;
  bool has_control = false;
  bool has_any = false;
  lexer::TokenKind last = lexer::TokenKind::Eof;
  for (usize i = start; i < lead.size(); ++i) {
    const Node& node = lead[i];
    if (node.kind == NodeKind::Comment) {
      continue;
    }
    has_any = true;
    if (node.kind == NodeKind::Group) {
      last = closer_of_group(node);
      continue;
    }
    last = node.token;
    switch (node.token) {
      case lexer::TokenKind::Match: has_match = true; break;
      case lexer::TokenKind::Struct: has_struct = true; break;
      case lexer::TokenKind::Enum: has_enum = true; break;
      case lexer::TokenKind::Impl: has_impl = true; break;
      case lexer::TokenKind::Spec: has_spec = true; break;
      case lexer::TokenKind::Extern: has_extern = true; break;
      case lexer::TokenKind::Fn:
      case lexer::TokenKind::Intrinsic:
        has_fn = true;
        break;
      case lexer::TokenKind::If:
      case lexer::TokenKind::Else:
      case lexer::TokenKind::While:
      case lexer::TokenKind::For:
      case lexer::TokenKind::Loop:
      case lexer::TokenKind::Unsafe:
      case lexer::TokenKind::Comp:
        has_control = true;
        break;
      default: break;
    }
  }

  if (has_match) return BraceRole::MatchBody;
  if (has_struct) return BraceRole::StructBody;
  if (has_enum) return BraceRole::EnumBody;
  if (has_impl) return BraceRole::ImplBody;
  if (has_spec) return BraceRole::SpecBody;
  if (has_extern) return BraceRole::ExternBody;
  if (has_fn) return BraceRole::FnBody;
  if (has_control) return BraceRole::Block;
  if (!has_any) return BraceRole::Block;
  if (forces_block_brace(last)) return BraceRole::Block;
  return BraceRole::Literal;
}

// Builds the delimiter tree. Each opener is consumed with its matching
// closer; an unclosed opener yields a group without a closer and a stray
// closer is kept as a leaf, so both halves of an imbalance survive.
class Builder {
 public:
  explicit Builder(const std::vector<Lexeme>& lexemes) : lexemes_(lexemes) {}

  Node build() {
    pos_ = 0;
    depth_ = 0;
    return build_sequence(lexer::TokenKind::Eof);
  }

 private:
  Node leaf(const Lexeme& lexeme) {
    Node node;
    node.kind = NodeKind::Token;
    node.token = lexeme.token;
    node.offset = lexeme.offset;
    node.length = lexeme.length;
    node.synthetic_semi = lexeme.synthetic_semi;
    node.newlines_before = lexeme.newlines_before;
    return node;
  }

  Node build_sequence(lexer::TokenKind expected_closer) {
    Node sequence;
    sequence.kind = NodeKind::Sequence;
    while (pos_ < lexemes_.size()) {
      const Lexeme& lexeme = lexemes_[pos_];
      if (lexeme.kind == Lexeme::Kind::End) {
        break;
      }
      if (lexeme.kind == Lexeme::Kind::Comment) {
        Node comment;
        comment.kind = NodeKind::Comment;
        comment.comment = lexeme.comment;
        comment.offset = lexeme.offset;
        comment.length = lexeme.length;
        comment.newlines_before = lexeme.newlines_before;
        sequence.kids.push_back(std::move(comment));
        ++pos_;
        continue;
      }
      if (is_opener(lexeme.token)) {
        sequence.kids.push_back(build_group(lexeme, sequence.kids));
        continue;
      }
      if (is_closer(lexeme.token)) {
        if (lexeme.token == expected_closer) {
          break;
        }
        sequence.kids.push_back(leaf(lexeme));
        ++pos_;
        continue;
      }
      sequence.kids.push_back(leaf(lexeme));
      ++pos_;
    }
    return sequence;
  }

  Node build_group(const Lexeme& opener, const std::vector<Node>& lead) {
    if (depth_ >= MAX_DEPTH) {
      // Out of budget: keep the opener as a leaf and let the rest read as
      // siblings rather than descend further.
      ++pos_;
      return leaf(opener);
    }
    Node group;
    group.kind = NodeKind::Group;
    group.group_role = group_role_of(opener.token);
    group.brace_role = group.group_role == GroupRole::Brace
                           ? classify_brace(lead)
                           : BraceRole::Literal;
    group.offset = opener.offset;
    group.length = opener.length;
    ++pos_;
    ++depth_;
    group.kids.push_back(build_sequence(matching_closer(opener.token)));
    --depth_;
    if (pos_ < lexemes_.size() &&
        lexemes_[pos_].kind == Lexeme::Kind::Token &&
        lexemes_[pos_].token == matching_closer(opener.token)) {
      group.has_closer = true;
      group.length = lexemes_[pos_].offset + lexemes_[pos_].length - group.offset;
      ++pos_;
    } else {
      group.has_closer = false;
    }
    return group;
  }

  const std::vector<Lexeme>& lexemes_;
  usize pos_ = 0;
  u32 depth_ = 0;
};

}  // namespace

Node build_cst(const std::vector<Lexeme>& lexemes) {
  Builder builder(lexemes);
  return builder.build();
}

}  // namespace alcy_fmt
