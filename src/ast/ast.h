// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
//   pattern     -> Pattern family  static     -> StaticItem
//   literal     -> Literal         const/use  -> ConstItem, UseItem
//   expressions -> Expr family     statements -> Stmt family, Block
//   fn          -> FnItem

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/union.h"
#include "fpag/base/vec.h"
#include "fpag/mem/arena.h"

namespace ast {

namespace details {
template <typename T>
using Idx = base::Idx<T, base::IdxBaseType>;
}

// Forward declarations for cross-references between node families.
struct Block;
struct Cond;
struct ExprNode;
struct ItemNode;
struct Literal;
struct Path;
struct PatternNode;
struct Stmt;
struct StmtNode;
struct TypeNode;

using LiteralIdx = details::Idx<Literal>;
using PathIdx = details::Idx<Path>;
using CondIdx = details::Idx<Cond>;
using BlockIdx = details::Idx<Block>;
using TypeIdx = details::Idx<TypeNode>;
using PatternIdx = details::Idx<PatternNode>;
using ExprIdx = details::Idx<ExprNode>;
using StmtIdx = details::Idx<StmtNode>;
using ItemIdx = details::Idx<ItemNode>;

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
  Array,
  Path,
  Ref,
};

struct Type {
  TypeKind kind;
  diag::Span span;
};

struct TypePrimitive {
  PrimitiveKind primitive;
};

struct TypeTuple {
  std::span<const TypeIdx> elements;
};

struct TypeArray {
  TypeIdx element = TypeIdx::invalid();
  u64 count = 0;
};

struct TypePath {
  PathIdx path = PathIdx::invalid();
  std::span<const TypeIdx> args;
};

struct TypeRef {
  bool is_mut;
  TypeIdx inner = TypeIdx::invalid();
};

// union TypePayload {
//   TypePayload() {}
//   TypePrimitive primitive;
//   TypeTuple tuple;
//   TypePath path;
//   TypeRef ref;
// };

struct TypeNode {
  TypeKind kind;
  diag::Span span;

  // Leaves members uninitialized; parsers set the active member
  // before pushing the node.
  using TypePayload =
      base::Union<TypePrimitive, TypeTuple, TypeArray, TypePath, TypeRef>;
  TypePayload payload;
};

struct PrimitiveType : Type {
  PrimitiveKind primitive;
};

struct TupleType : Type {
  std::span<Type* const> elements;
};

struct PathType : Type {
  const Path* path;
  // Empty without "<...>" arguments; the count must match the
  // declaration's parameter list for a generic type.
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

struct PatternIdent {
  Ident name;
};

struct PatternLiteral {
  LiteralIdx value = LiteralIdx::invalid();
};

struct PatternTuple {
  PathIdx path = PathIdx::invalid();
  std::span<const PatternIdx> elements;
};

struct FieldPattern {
  Ident name;
  PatternIdx pattern = PatternIdx::invalid();
};

struct PatternStruct {
  PathIdx path = PathIdx::invalid();
  std::span<const FieldPattern> fields;
};

struct PatternRef {
  bool is_mut;
  PatternIdx inner = PatternIdx::invalid();
};

struct PatternOr {
  std::span<const PatternIdx> alternatives;
};

// TODO: PatternPayload contains 2 PatternIdent,
// so it cannot be converted to base::Union.
union PatternPayload {
  PatternPayload() {}
  PatternIdent ident;
  PatternIdent mut_ident;
  PatternLiteral literal;
  PatternTuple tuple;
  PatternStruct strukt;
  PatternRef ref;
  PatternOr or_pat;
};

struct PatternNode {
  PatternKind kind;
  diag::Span span;

  // Leaves members uninitialized; parsers set the active member
  // before pushing the node.
  PatternPayload payload;
};

struct WildcardPattern : Pattern {};

struct IdentPattern : Pattern {
  Ident name;
};

struct MutIdentPattern : Pattern {
  Ident name;
};

struct LiteralPattern : Pattern {
  LiteralIdx value = LiteralIdx::invalid();
};

struct TuplePattern : Pattern {
  // Null for plain tuple patterns; set for enum tuple variants.
  const Path* path;
  std::span<Pattern* const> elements;
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
  Array,
  Unary,
  Borrow,
  Deref,
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

struct ExprLiteral {
  LiteralIdx value = LiteralIdx::invalid();
};

struct ExprPath {
  PathIdx idx = PathIdx::invalid();
};

struct ExprFieldInit {
  Ident name;
  ExprIdx value = ExprIdx::invalid();
};

struct ExprStruct {
  PathIdx path = PathIdx::invalid();
  std::span<const ExprFieldInit> init;
  ExprIdx base_expr = ExprIdx::invalid();
};

struct ExprTuple {
  std::span<const ExprIdx> elements;
};

// Fixed array literal: a list `[a, b, c]`, or a repeat `[e; N]`
// with an invalid `repeat` for lists and zero count.
struct ExprArray {
  std::span<const ExprIdx> elements;
  ExprIdx repeat = ExprIdx::invalid();
  u64 count = 0;
};

struct ExprUnary {
  UnaryOp op;
  ExprIdx inner = ExprIdx::invalid();
};

struct ExprBorrow {
  bool is_mut;
  ExprIdx inner = ExprIdx::invalid();
};

// `*place`, the place a reference addresses.
struct ExprDeref {
  ExprIdx inner = ExprIdx::invalid();
};

struct ExprBinary {
  BinaryOp op;
  ExprIdx lhs = ExprIdx::invalid();
  ExprIdx rhs = ExprIdx::invalid();
};

struct ExprCast {
  ExprIdx inner = ExprIdx::invalid();
  TypeIdx type = TypeIdx::invalid();
};

struct ExprCall {
  ExprIdx callee = ExprIdx::invalid();
  // Explicit type arguments from turbofish syntax `f::<T>(...)`;
  // empty when the call relies on inference.
  std::span<const TypeIdx> type_args;
  std::span<const ExprIdx> args;
};

struct ExprMethodCall {
  ExprIdx receiver = ExprIdx::invalid();
  Ident name;
  std::span<const ExprIdx> args;
};

struct ExprField {
  ExprIdx receiver = ExprIdx::invalid();
  Ident name;
};

struct ExprIndex {
  ExprIdx receiver = ExprIdx::invalid();
  ExprIdx index = ExprIdx::invalid();
};

struct ExprQuestion {
  ExprIdx inner = ExprIdx::invalid();
};

struct ExprIf {
  CondIdx cond = CondIdx::invalid();
  BlockIdx then_block = BlockIdx::invalid();
  BlockIdx else_block = BlockIdx::invalid();
};

struct ExprMatchArm {
  PatternIdx pattern = PatternIdx::invalid();
  ExprIdx body = ExprIdx::invalid();
};

struct ExprMatch {
  ExprIdx scrutinee = ExprIdx::invalid();
  std::span<const ExprMatchArm> arms;
};

struct ExprLoop {
  BlockIdx body = BlockIdx::invalid();
};

struct ExprWhile {
  CondIdx cond = CondIdx::invalid();
  BlockIdx body = BlockIdx::invalid();
};

struct ExprBlock {
  BlockIdx block = BlockIdx::invalid();
  bool is_comp = false;
};

struct ExprReturn {
  ExprIdx value = ExprIdx::invalid();
};

struct ExprRange {
  ExprIdx start = ExprIdx::invalid();
  ExprIdx end = ExprIdx::invalid();
  bool inclusive;
};

// union ExprPayload {
//   ExprPayload() {}
//   ExprLiteral literal;
//   ExprPath path;
//   ExprStruct strukt;
//   ExprTuple tuple;
//   ExprUnary unary;
//   ExprBorrow borrow;
//   ExprBinary binary;
//   ExprCast cast;
//   ExprCall call;
//   ExprMethodCall method_call;
//   ExprField field;
//   ExprIndex index;
//   ExprQuestion question;
//   ExprIf if_expr;
//   ExprMatch match;
//   ExprLoop loop;
//   ExprWhile while_expr;
//   ExprBlock block;
//   ExprReturn ret;
//   ExprRange range;
// };

struct ExprNode {
  ExprKind kind;
  diag::Span span;

  // Leaves members uninitialized; parsers set the active member
  // before pushing the node.
  using ExprPayload = base::Union<ExprLiteral,
                                  ExprPath,
                                  ExprStruct,
                                  ExprTuple,
                                  ExprArray,
                                  ExprUnary,
                                  ExprBorrow,
                                  ExprDeref,
                                  ExprBinary,
                                  ExprCast,
                                  ExprCall,
                                  ExprMethodCall,
                                  ExprField,
                                  ExprIndex,
                                  ExprQuestion,
                                  ExprIf,
                                  ExprMatch,
                                  ExprLoop,
                                  ExprWhile,
                                  ExprBlock,
                                  ExprReturn,
                                  ExprRange>;
  ExprPayload payload;
};

struct LiteralExpr : Expr {
  LiteralIdx value = LiteralIdx::invalid();
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

struct BorrowExpr : Expr {
  bool is_mut;
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
  PatternIdx pattern = PatternIdx::invalid();
  ExprIdx init = ExprIdx::invalid();
  ExprIdx value = ExprIdx::invalid();
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
  std::span<const StmtIdx> statements;
  // Null for blocks ending in a statement; "{}" has no statements and
  // no value.
  ExprIdx value = ExprIdx::invalid();
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

struct StmtDecl {
  PatternIdx pattern = PatternIdx::invalid();
  TypeIdx type = TypeIdx::invalid();
  ExprIdx init = ExprIdx::invalid();
  bool is_comp = false;
};

struct StmtReassign {
  ExprIdx place = ExprIdx::invalid();
  bool compound;
  BinaryOp op;
  ExprIdx value = ExprIdx::invalid();
};

struct StmtExpr {
  ExprIdx value = ExprIdx::invalid();
};

// union StmtPayload {
//   StmtPayload() {}
//   StmtDecl decl;
//   StmtReassign reassign;
//   StmtExpr expr;
// };

struct StmtNode {
  StmtKind kind;
  diag::Span span;

  // Leaves members uninitialized; parsers set the active member
  // before pushing the node.
  using StmtPayload = base::Union<StmtDecl, StmtReassign, StmtExpr>;
  StmtPayload payload;
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
  Intrinsic,
  Struct,
  Enum,
  Impl,
  Static,
  Const,
  Use,
};

struct Item {
  ItemKind kind;
  diag::Span span;
  bool is_pub;
};

struct ItemFnParam {
  PatternIdx pattern = PatternIdx::invalid();
  TypeIdx type = TypeIdx::invalid();
  bool is_comp = false;
};

struct ItemFn {
  Ident name;
  // Type parameters; empty for a non-generic function.
  std::span<const Ident> generic;
  std::span<const ItemFnParam> params;
  TypeIdx return_type = TypeIdx::invalid();
  BlockIdx body = BlockIdx::invalid();
};

// A compiler-provided function: signature without a body. Calls
// check like ordinary calls; lowering maps known names to runtime
// hooks or IR operations.
struct ItemIntrinsic {
  Ident name;
  // Type parameters; empty for a non-generic intrinsic. A generic
  // intrinsic must admit exactly one binding per call, so its
  // parameters are recoverable from the argument types alone.
  std::span<const Ident> generic;
  std::span<const ItemFnParam> params;
  TypeIdx return_type = TypeIdx::invalid();
};

struct ItemStructField {
  Ident name;
  TypeIdx type = TypeIdx::invalid();
};

struct ItemStruct {
  Ident name;
  std::span<const Ident> params;
  std::span<const ItemStructField> fields;
};

struct ItemEnumVariant {
  Ident name;
  std::span<const TypeIdx> fields;
};

struct ItemEnum {
  Ident name;
  std::span<const Ident> params;
  std::span<const ItemEnumVariant> variants;
};

struct ItemImpl {
  std::span<const Ident> params;
  TypeIdx type = TypeIdx::invalid();
  std::span<const ItemIdx> methods;
};

struct ItemStatic {
  Ident name;
  TypeIdx type = TypeIdx::invalid();
  ExprIdx init = ExprIdx::invalid();
};

struct ItemConst {
  Ident name;
  TypeIdx type = TypeIdx::invalid();
  ExprIdx init = ExprIdx::invalid();
};

struct ItemUse {
  PathIdx path = PathIdx::invalid();
  bool has_alias;
  Ident alias;
};

// union ItemPayload {
//   ItemPayload() {}
//   ItemFn fn_item;
//   ItemStruct struct_item;
//   ItemEnum enum_item;
//   ItemImpl impl;
//   ItemStatic static_item;
//   ItemConst const_item;
//   ItemUse use_item;
// };

struct ItemNode {
  ItemKind kind;
  diag::Span span;
  bool is_pub;

  // Leaves members uninitialized; parsers set the active member
  // before pushing the node.
  using ItemPayload = base::Union<ItemFn,
                                  ItemIntrinsic,
                                  ItemStruct,
                                  ItemEnum,
                                  ItemImpl,
                                  ItemStatic,
                                  ItemConst,
                                  ItemUse>;
  ItemPayload payload;

  std::string_view name() const {
    using I = ItemKind;
    switch (kind) {
      case I::Struct: return payload.get<ItemStruct>().name.name;
      case I::Enum: return payload.get<ItemEnum>().name.name;
      case I::Fn: return payload.get<ItemFn>().name.name;
      case I::Intrinsic: return payload.get<ItemIntrinsic>().name.name;
      case I::Static: return payload.get<ItemStatic>().name.name;
      case I::Const: return payload.get<ItemConst>().name.name;
      case I::Use:   // return item.get<ItemUse>().name.name;
      case I::Impl:  // return item.get<ItemImpl>().name.name;
      default: return "unknown";
    }
  }
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

// Arena of AST nodes: one table per node type, addressed by the
// index types above. Edges between nodes are indices, so passes
// never downcast. Span arrays and identifier spellings live in
// `spans`, reserved upfront; node tables own the nodes themselves.
struct AstArena {
  AstArena() { spans.reserve(1u << 20); }

  template <typename T>
  using Alloc = std::allocator<T>;

  mem::Arena spans;

  base::Vec<Literal, LiteralIdx, Alloc<Literal>> literals;
  base::Vec<Path, PathIdx, Alloc<Path>> paths;
  base::Vec<Cond, CondIdx, Alloc<Cond>> conds;
  base::Vec<Block, BlockIdx, Alloc<Block>> blocks;
  base::Vec<TypeNode, TypeIdx, Alloc<TypeNode>> types;
  base::Vec<PatternNode, PatternIdx, Alloc<PatternNode>> patterns;
  base::Vec<ExprNode, ExprIdx, Alloc<ExprNode>> exprs;
  base::Vec<StmtNode, StmtIdx, Alloc<StmtNode>> stmts;
  base::Vec<ItemNode, ItemIdx, Alloc<ItemNode>> items;
};

}  // namespace ast
