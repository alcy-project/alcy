// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

// Abstract syntax tree nodes: plain data allocated in a per-file arena,
// consumed by the parser and later stages. Every node carries its source
// span. Polymorphism is manual (kind-tagged structs embedding a base as
// their first member, dispatched by switching on the kind): the project
// forbids vtables, so there are no virtual methods anywhere here.
//
// Grammar coverage (docs/spec/grammar.md):
//   path        -> Path            params     -> FnParam
//   primitive   -> PrimitiveKind   struct     -> StructItem, StructField
//   tuple type  -> TupleType       enum       -> EnumItem, EnumVariant
//   ref type    -> RefType         impl       -> ImplItem
//   pattern     -> Pattern family  mod/static -> ModItem, StaticItem
//   literal     -> Literal         const/use  -> ConstItem, UseItem
//   expressions -> Expr family     statements -> Stmt family, Block
//   fn          -> FnItem

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"

namespace ast {

// Forward declarations for cross-references between node families.
struct Block;
struct Stmt;

// Copies a scratch list into the arena; the returned span borrows arena
// storage for the arena's lifetime.
template <typename T>
std::span<T> copy_to_arena(mem::Arena& arena, const std::vector<T>& items) {
  if (items.empty()) {
    return {};
  }
  T* const out =
      static_cast<T*>(arena.alloc(sizeof(T) * items.size(), alignof(T)));
  std::uninitialized_copy(items.begin(), items.end(), out);
  return std::span<T>(out, items.size());
}

// Identifiers and paths

struct Ident {
  std::string_view name;
  diag::Span span;
};

struct Path {
  std::span<const Ident> segments;
  diag::Span span;
};

// Types

enum class PrimitiveKind : u8 {
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
};

enum class TypeKind : u8 {
  Primitive,
  Unit,
  Never,
  Str,
  Tuple,
  Path,
  Ref,
};

struct Type {
  TypeKind kind;
  diag::Span span;
};

struct PrimitiveType : Type {
  PrimitiveKind primitive;
};

struct TupleType : Type {
  std::span<Type* const> elements;
};

struct PathType : Type {
  const Path* path;
  // Empty without "<...>" arguments (blessed generics only).
  std::span<Type* const> args;
};

struct RefType : Type {
  bool is_mut;
  const Type* inner;
};

// Literals

enum class LiteralKind : u8 {
  Integer,
  Float,
  String,
  Char,
  Bool,
};

struct Literal {
  LiteralKind kind;
  diag::Span span;
  // Source spelling (including suffixes and quotes); the semantic value
  // is computed on demand by later stages.
  std::string_view spelling;
};

// Patterns

enum class PatternKind : u8 {
  Wildcard,
  Ident,
  MutIdent,
  Literal,
  Tuple,
  Struct,
  Ref,
  Or,
};

struct Pattern {
  PatternKind kind;
  diag::Span span;
};

struct WildcardPattern : Pattern {};

struct IdentPattern : Pattern {
  Ident name;
};

struct MutIdentPattern : Pattern {
  Ident name;
};

struct LiteralPattern : Pattern {
  const Literal* value;
};

struct TuplePattern : Pattern {
  // Null for plain tuple patterns; set for enum tuple variants.
  const Path* path;
  std::span<Pattern* const> elements;
};

struct FieldPattern {
  Ident name;
  const Pattern* pattern;
};

struct StructPattern : Pattern {
  const Path* path;
  std::span<const FieldPattern> fields;
};

struct RefPattern : Pattern {
  bool is_mut;
  const Pattern* inner;
};

struct OrPattern : Pattern {
  std::span<Pattern* const> alternatives;
};

// Expressions

enum class UnaryOp : u8 {
  Neg,
  Not,
  BitNot,
};

enum class BinaryOp : u8 {
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Pow,
  BitAnd,
  BitOr,
  BitXor,
  Shl,
  Shr,
  Eq,
  NotEq,
  Gt,
  Lt,
  GtEq,
  LtEq,
  And,
  Or,
};

enum class ExprKind : u8 {
  Literal,
  Path,
  Struct,
  Tuple,
  Unary,
  Binary,
  Cast,
  Call,
  MethodCall,
  Field,
  Index,
  Question,
  If,
  Match,
  Loop,
  While,
  Block,
  Return,
  Break,
  Continue,
  Range,
};

struct Expr {
  ExprKind kind;
  diag::Span span;
};

struct LiteralExpr : Expr {
  const Literal* value;
};

struct PathExpr : Expr {
  const Path* path;
};

struct FieldInit {
  Ident name;
  const Expr* value;
};

struct StructExpr : Expr {
  const Path* path;
  std::span<const FieldInit> init;
  // Null without a "..base" clause.
  const Expr* base_expr;
};

struct TupleExpr : Expr {
  // Empty elements denote the unit value "()".
  std::span<Expr* const> elements;
};

struct UnaryExpr : Expr {
  UnaryOp op;
  const Expr* inner;
};

struct BinaryExpr : Expr {
  BinaryOp op;
  const Expr* lhs;
  const Expr* rhs;
};

struct CastExpr : Expr {
  const Expr* inner;
  const Type* type;
};

struct CallExpr : Expr {
  const Expr* callee;
  std::span<Expr* const> args;
};

struct MethodCallExpr : Expr {
  const Expr* receiver;
  Ident name;
  std::span<Expr* const> args;
};

struct FieldExpr : Expr {
  const Expr* receiver;
  // Tuple ".0" access synthesizes an Ident holding the digits.
  Ident name;
};

struct IndexExpr : Expr {
  const Expr* receiver;
  const Expr* index;
};

struct QuestionExpr : Expr {
  const Expr* inner;
};

// An `if`/`while` condition: either a plain boolean expression or a
// pattern declaration (`if pat := expr`).
struct Cond {
  bool is_pattern;
  const Pattern* pattern;
  const Expr* init;
  const Expr* value;
};

struct IfExpr : Expr {
  const Cond* cond;
  const Block* then_block;
  // Null without an else clause.
  const Block* else_block;
};

struct MatchArm {
  const Pattern* pattern;
  const Expr* body;
};

struct MatchExpr : Expr {
  const Expr* scrutinee;
  std::span<const MatchArm> arms;
};

struct LoopExpr : Expr {
  const Block* body;
};

struct WhileExpr : Expr {
  const Cond* cond;
  const Block* body;
};

struct Block {
  diag::Span span;
  std::span<Stmt* const> statements;
  // Null for blocks ending in a statement; "{}" has no statements and
  // no value.
  const Expr* value;
};

struct BlockExpr : Expr {
  const Block* block;
};

struct ReturnExpr : Expr {
  // Null for bare `ret`.
  const Expr* value;
};

struct RangeExpr : Expr {
  // Null bounds denote an absent endpoint; both null is full "..".
  const Expr* start;
  const Expr* end;
  // Meaningful only with an end bound: true for "..=", false for "..<".
  bool inclusive;
};

struct BreakExpr : Expr {};

struct ContinueExpr : Expr {};

// Statements

enum class StmtKind : u8 {
  Decl,
  Reassign,
  Expr,
};

struct Stmt {
  StmtKind kind;
  diag::Span span;
};

struct DeclStmt : Stmt {
  const Pattern* pattern;
  // Null without a type ascription.
  const Type* type;
  const Expr* init;
};

struct ReassignStmt : Stmt {
  const Expr* place;
  // Plain "=" when false; otherwise the combined operator.
  bool compound;
  BinaryOp op;
  const Expr* value;
};

struct ExprStmt : Stmt {
  const Expr* value;
};

// Items

enum class ItemKind : u8 {
  Fn,
  Struct,
  Enum,
  Impl,
  Mod,
  Static,
  Const,
  Use,
};

struct Item {
  ItemKind kind;
  diag::Span span;
  bool is_pub;
};

struct FnParam {
  const Pattern* pattern;
  const Type* type;
};

struct FnItem : Item {
  Ident name;
  std::span<const FnParam> params;
  // Null without a return type (means "()").
  const Type* return_type;
  const Block* body;
};

struct StructField {
  Ident name;
  const Type* type;
};

struct StructItem : Item {
  Ident name;
  std::span<const StructField> fields;
};

struct EnumVariant {
  Ident name;
  // Empty for unit variants.
  std::span<Type* const> fields;
};

struct EnumItem : Item {
  Ident name;
  std::span<const EnumVariant> variants;
};

struct ImplItem : Item {
  const Type* type;
  std::span<FnItem* const> methods;
};

struct ModItem : Item {
  Ident name;
  // Null for file modules (`mod foo;`); set for inline modules.
  std::span<Item* const> items;
  bool is_inline;
};

struct StaticItem : Item {
  Ident name;
  const Type* type;
  const Expr* init;
};

struct ConstItem : Item {
  Ident name;
  const Type* type;
  const Expr* init;
};

struct UseItem : Item {
  const Path* path;
  bool has_alias;
  Ident alias;
};

}  // namespace ast
