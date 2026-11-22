// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cst/cst.h"

#include <utility>
#include <vector>

#include "base/nesting.h"
#include "cst/lex.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"

namespace cst {

namespace {

bool is_opener(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::LParen ||
         kind == lexer::TokenKind::LBracket || kind == lexer::TokenKind::LBrace;
}

bool is_closer(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::RParen ||
         kind == lexer::TokenKind::RBracket || kind == lexer::TokenKind::RBrace;
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
    case lexer::TokenKind::Colon: return true;
    default: return false;
  }
}

// The header in front of a brace: the run of siblings since the last
// separator, flat because every nested delimiter is already a node of its
// own. The summary catches up with the siblings that landed since the
// previous brace, so a run is read once instead of once per brace.
class Header {
 public:
  [[gnu::always_inline]] void add(const Node& node) {
    if (node.kind == NodeKind::Comment) {
      return;
    }
    if (node.kind == NodeKind::Token &&
        node.token == lexer::TokenKind::Semicolon) {
      *this = Header{};
      return;
    }
    has_any_ = true;
    if (node.kind == NodeKind::Group) {
      last_ = closer_of_group(node);
      has_read_ = true;
      return;
    }
    last_ = node.token;
    has_read_ = node.token != lexer::TokenKind::Error;
    switch (node.token) {
      case lexer::TokenKind::Match: has_match_ = true; break;
      case lexer::TokenKind::Struct: has_struct_ = true; break;
      case lexer::TokenKind::Enum: has_enum_ = true; break;
      case lexer::TokenKind::Impl: has_impl_ = true; break;
      case lexer::TokenKind::Spec: has_spec_ = true; break;
      case lexer::TokenKind::Extern: has_extern_ = true; break;
      case lexer::TokenKind::Test: has_test_ = true; break;
      case lexer::TokenKind::Fn:
      case lexer::TokenKind::Intrinsic: has_fn_ = true; break;
      case lexer::TokenKind::If:
      case lexer::TokenKind::Else:
      case lexer::TokenKind::While:
      case lexer::TokenKind::For:
      case lexer::TokenKind::Loop:
      case lexer::TokenKind::Unsafe:
      case lexer::TokenKind::Comp: has_control_ = true; break;
      default: break;
    }
  }

  BraceRole brace_role() const {
    if (has_test_) {
      // The keyword directly owns the brace of a block of items; after a
      // parameter list it heads an entry, whose brace holds statements
      // the way a function body does.
      if (last_ == lexer::TokenKind::Test) {
        return BraceRole::TestBody;
      }
      if (last_ == lexer::TokenKind::RParen) {
        return BraceRole::FnBody;
      }
    }
    if (has_match_) {
      return BraceRole::MatchBody;
    }
    if (has_struct_) {
      return BraceRole::StructBody;
    }
    if (has_enum_) {
      return BraceRole::EnumBody;
    }
    if (has_impl_) {
      return BraceRole::ImplBody;
    }
    if (has_spec_) {
      return BraceRole::SpecBody;
    }
    if (has_extern_) {
      return BraceRole::ExternBody;
    }
    if (has_fn_) {
      return BraceRole::FnBody;
    }
    // A brace the unsafe or comp keyword opens is the expression the
    // keyword owns, however deeply the line around it is spelled.
    if (last_ == lexer::TokenKind::Unsafe || last_ == lexer::TokenKind::Comp) {
      return BraceRole::ExprBlock;
    }
    if (has_control_) {
      return BraceRole::Block;
    }
    if (!has_any_) {
      return BraceRole::Block;
    }
    if (!has_read_) {
      return BraceRole::Block;
    }
    if (forces_block_brace(last_)) {
      return BraceRole::Block;
    }
    return BraceRole::Literal;
  }

 private:
  bool has_match_;
  bool has_struct_;
  bool has_enum_;
  bool has_impl_;
  bool has_spec_;
  bool has_extern_;
  bool has_test_;
  bool has_fn_;
  bool has_control_;
  bool has_any_;
  // Whether the header holds anything the layout itself wrote down: a
  // real token or a closed delimiter. A header of nothing but unread
  // bytes classifies as a block, because whether a separator stood in
  // front of it is exactly what the layout cannot promise to carry
  // into the next pass.
  bool has_read_;
  lexer::TokenKind last_;
};

// One open sequence: its header summary and how many of its siblings that
// summary has read. A frame reads as the empty summary only through Frame{},
// which the sequence opening it applies before it pushes anything.
struct Frame {
  Header header;
  usize absorbed;
};

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
    Frame& frame = frames_[depth_];
    frame = Frame{};
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
        if (lexeme.token == lexer::TokenKind::LBrace) {
          catch_up(frame, sequence.kids);
        }
        Node group = build_group(lexeme, frame.header);
        sequence.kids.push_back(std::move(group));
        continue;
      }
      if (is_closer(lexeme.token)) {
        if (lexeme.token == expected_closer) {
          break;
        }
        Node stray = leaf(lexeme);
        sequence.kids.push_back(std::move(stray));
        ++pos_;
        continue;
      }
      Node node = leaf(lexeme);
      sequence.kids.push_back(std::move(node));
      ++pos_;
    }
    return sequence;
  }

  // Folds the siblings appended since the last brace into the summary the
  // next brace reads.
  static void catch_up(Frame& frame, const std::vector<Node>& kids) {
    for (; frame.absorbed < kids.size(); ++frame.absorbed) {
      frame.header.add(kids[frame.absorbed]);
    }
  }

  Node build_group(const Lexeme& opener, const Header& header) {
    if (depth_ >= base::MAX_NESTING) {
      // Out of budget: keep the opener as a leaf and let the rest read as
      // siblings rather than descend further.
      ++pos_;
      return leaf(opener);
    }
    Node group;
    group.kind = NodeKind::Group;
    group.group_role = group_role_of(opener.token);
    group.brace_role = group.group_role == GroupRole::Brace
                           ? header.brace_role()
                           : BraceRole::Literal;
    group.offset = opener.offset;
    group.length = opener.length;
    group.newlines_before = opener.newlines_before;
    ++pos_;
    ++depth_;
    group.kids.push_back(build_sequence(matching_closer(opener.token)));
    --depth_;
    if (pos_ < lexemes_.size() && lexemes_[pos_].kind == Lexeme::Kind::Token &&
        lexemes_[pos_].token == matching_closer(opener.token)) {
      group.has_closer = true;
      group.length =
          lexemes_[pos_].offset + lexemes_[pos_].length - group.offset;
      ++pos_;
    } else {
      group.has_closer = false;
    }
    return group;
  }

  const std::vector<Lexeme>& lexemes_;
  // One frame per open sequence; the depth budget bounds the stack. The
  // array stays unset so a shallow parse does not zero the depth it never
  // opens.
  Frame frames_[base::MAX_NESTING + 1];
  usize pos_ = 0;
  u32 depth_ = 0;
};

}  // namespace

Node build_cst(const std::vector<Lexeme>& lexemes) {
  Builder builder(lexemes);
  return builder.build();
}

}  // namespace cst
