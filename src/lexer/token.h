// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "diag/span.h"
#include "fpag/base/numeric.h"

namespace lexer {

// Token kinds, frozen with docs/spec/keywords.md. Additions require a
// specification update. Reserved words lex as distinct kinds so the
// parser can reject them with guidance instead of mistaking them for
// identifiers.
enum class TokenKind : u8 {
  // Declarations.
  Fn,
  Struct,
  Enum,
  Impl,
  Static,
  Pub,
  Const,
  Mut,
  Use,

  // Control flow.
  If,
  Else,
  Loop,
  While,
  Break,
  Continue,
  Ret,
  Match,

  // Paths and casts.
  Package,
  Self,
  Super,
  SelfType,
  As,

  // Primitive types.
  I8,
  I16,
  I32,
  I64,
  I128,
  Isize,
  U8,
  U16,
  U32,
  U64,
  U128,
  Usize,
  F32,
  F64,
  Bool,
  Str,

  // Reserved words (parsed, rejected with guidance).
  Async,
  Await,
  Union,
  Register,
  Comp,
  Extern,
  Unsafe,
  For,
  In,
  Where,
  Dyn,

  // Literals.
  Integer,
  Float,
  String,
  Char,
  True,
  False,

  // Operators.
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  StarStar,
  Amp,
  Pipe,
  Caret,
  Tilde,
  LessLess,
  GreaterGreater,
  PlusEq,
  MinusEq,
  StarEq,
  SlashEq,
  PercentEq,
  StarStarEq,
  AmpEq,
  PipeEq,
  CaretEq,
  LessLessEq,
  GreaterGreaterEq,
  ColonEq,
  Eq,
  EqEq,
  Bang,
  AmpAmp,
  PipePipe,
  BangEq,
  Greater,
  Less,
  GreaterEq,
  LessEq,
  Arrow,
  FatArrow,
  Colon,
  ColonColon,
  Comma,
  Dot,
  DotDot,
  DotDotEq,
  DotDotLess,

  // Delimiters.
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Question,
  Semicolon,
  Hash,        // Reserved for future attributes; rejected with guidance.
  Underscore,  // Wildcard patterns and explicit discards.

  // Identifiers.
  Ident,

  // Special.
  DocComment,
  Error,
  Eof,
};

// A lexical token: its kind plus the source span it was read from.
// Literal spellings live in the source text and are re-read from the
// SourceManager on demand, so tokens stay small and POD-like.
struct Token {
  TokenKind kind = TokenKind::Eof;
  diag::Span span;
};

}  // namespace lexer
