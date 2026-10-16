// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "fmt/src/lex.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"

namespace alcy_fmt {

// What a node is. The tree nests delimiters only: everything else is a
// leaf, so a construct the grammar would reject still has a place in the
// tree and nothing is dropped.
enum class NodeKind : u8 { Sequence, Group, Token, Comment };

// What a delimiter pair is.
enum class GroupRole : u8 { Paren, Bracket, Brace };

// What a brace pair holds. The layout differs enough between a block of
// statements, a list of members, a run of match arms, and a value that a
// single "brace" case would have to guess at print time; classifying once
// here keeps that decision next to the reading of the source.
enum class BraceRole : u8 {
  Block,
  // A block that is an expression: unsafe and comp blocks wrap a value
  // the way parentheses do, so they lay out like one.
  ExprBlock,
  FnBody,
  StructBody,
  EnumBody,
  ImplBody,
  SpecBody,
  ExternBody,
  MatchBody,
  Literal,
};

// A concrete syntax tree node.
//
// A Sequence holds children in source order. A Group holds exactly one
// Sequence, its contents. A Token and a Comment are leaves. The source
// range is kept for leaf text; a Group records where it opened, so a
// missing closer is representable rather than repaired.
struct Node {
  NodeKind kind = NodeKind::Sequence;
  std::vector<Node> kids;

  GroupRole group_role = GroupRole::Paren;
  BraceRole brace_role = BraceRole::Literal;
  bool has_closer = true;

  lexer::TokenKind token = lexer::TokenKind::Eof;
  CommentKind comment = CommentKind::Line;

  u32 offset = 0;
  u32 length = 0;
  bool synthetic_semi = false;
  usize newlines_before = 0;
};

// Builds the tree for a token stream. The result is never empty: an empty
// input yields a File sequence with no children.
Node build_cst(const std::vector<Lexeme>& lexemes);

}  // namespace alcy_fmt
