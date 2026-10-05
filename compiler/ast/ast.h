// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

// Abstract syntax tree nodes: plain data owned by a per-file arena,
// consumed by the parser and later stages. Every node carries its source
// span. Polymorphism is manual: a `kind` field selects the active member
// of a `base::Union` payload, and every pass dispatches on the kind
// before reading `payload.get<T>()`. Vtables are forbidden
// (PRINCIPLES.md), so nothing here has virtual methods.
//
// Grammar coverage (docs/spec/grammar.md):
//   path        -> Path            params     -> ItemFnParam
//   primitive   -> PrimitiveKind   struct     -> ItemStruct, ItemStructField
//   tuple type  -> TypeTuple       enum       -> ItemEnum, ItemEnumVariant
//   ref type    -> TypeRef         impl       -> ItemImpl
//   pattern     -> PatternNode     static     -> ItemStatic
//   literal     -> Literal         const/use  -> ItemConst, ItemUse
//   expressions -> ExprNode        statements -> StmtNode, Block
//   fn          -> ItemFn          closure    -> ExprClosure
//   func type   -> TypeFunc

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "ast/node_vec.h"
#include "ast/span_arena.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/union.h"
#include "fpag/build/build_flag.h"

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
// storage for the arena's lifetime. A spent arena answers with an empty
// span and marks itself, which is what the parser stops on: the empty
// span is not read as a list of no items.
template <typename T, typename Arena>
std::span<T> copy_to_arena(Arena& arena, const std::vector<T>& items) {
  if (items.empty()) {
    return {};
  }
  T* const out =
      static_cast<T*>(arena.alloc(sizeof(T) * items.size(), alignof(T)));
  if (out == nullptr) [[unlikely]] {
    return {};
  }
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
  Slice,
  Path,
  Ref,
  Func,
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

struct TypeSlice {
  TypeIdx element = TypeIdx::invalid();
};

struct TypePath {
  PathIdx path = PathIdx::invalid();
  std::span<const TypeIdx> args;
};

struct TypeRef {
  bool is_mut;
  TypeIdx inner = TypeIdx::invalid();
};

// A function type `(A, B) -> R`: structural, with captures erased.
// The parameter list is empty for `() -> R`.
struct TypeFunc {
  std::span<const TypeIdx> params;
  TypeIdx ret = TypeIdx::invalid();
};

struct TypeNode {
  TypeKind kind;
  diag::Span span;

  // Leaves members uninitialized; parsers set the active member
  // before pushing the node.
  using TypePayload = base::Union<TypePrimitive,
                                  TypeTuple,
                                  TypeArray,
                                  TypeSlice,
                                  TypePath,
                                  TypeRef,
                                  TypeFunc>;
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
  // Source spelling of the magnitude (including suffixes and quotes),
  // without a leading `-`; the semantic value is computed on demand by
  // later stages.
  std::string_view spelling;
  // A `-` preceded this literal. Patterns keep the sign here rather
  // than in `spelling`, so the magnitude parses the same either way and
  // only the value differs. Expressions spell negation as `ExprUnary`.
  bool is_negative = false;
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
  // The `for` desugar sets this on the generated `Option::Some(pat)`:
  // the item pattern must match every item, so a refutable one is a
  // check-time error rather than a filter.
  bool for_pattern = false;
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
  Closure,
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
  // Turbofish arguments from `Name::<T>::member`; empty otherwise.
  std::span<const TypeIdx> type_args;
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
  // Resolves through spec impls only, never inherent ones. The `for`
  // desugar sets this on the generated `next` call so the cursor must
  // reach the method through `Iterator`, not through a same-named
  // inherent method.
  bool spec_only = false;
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
  // An `unsafe { ... }` block: operations that need the gate may
  // appear inside it, and its value is the block's.
  bool is_unsafe = false;
};

struct ExprReturn {
  ExprIdx value = ExprIdx::invalid();
};

struct ExprRange {
  ExprIdx start = ExprIdx::invalid();
  ExprIdx end = ExprIdx::invalid();
  bool inclusive;
};

// One closure parameter: a name or `_`, `mut` as in declaration
// patterns, and an optional type. Destructuring waits.
struct ClosureParam {
  Ident name;
  bool is_mut = false;
  bool is_wildcard = false;
  TypeIdx type = TypeIdx::invalid();
};

// How a closure takes a name from its enclosing scope: by value (a
// move, or a copy for a Copy type), by shared reference, or by
// exclusive reference.
enum class CaptureMode : u8 {
  Move,
  Shared,
  Mut,
};

// One entry of a capture list: the local it names, and how the
// closure takes it.
struct Capture {
  Ident name;
  CaptureMode mode = CaptureMode::Move;
};

// A closure `(params) -> body` with an optional capture list.
// `captures` names locals only; an empty list and no list both
// mean the closure sees nothing outside its parameters.
struct ExprClosure {
  // Like the parameters below: the desugarer rewrites each name to
  // its bound spelling.
  std::span<Capture> captures;
  // The desugarer rewrites a parameter's name to its bound spelling, so
  // the span is mutable even though only the parser and that pass write
  // through it.
  std::span<ClosureParam> params;
  ExprIdx body = ExprIdx::invalid();
};

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
                                  ExprRange,
                                  ExprClosure>;
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
  // Invalid for blocks ending in a statement; "{}" has no statements
  // and no value.
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
  Spec,
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
  // An `unsafe fn`: calling it is an operation that needs the gate.
  bool is_unsafe = false;
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
  // The precondition the compiler cannot check: the checker verifies
  // this against the intrinsic set the way it verifies the shape.
  bool is_unsafe = false;
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
  // The implemented spec, as a type path with arguments; invalid for
  // an inherent impl. `impl Display for Point` parses the spec side
  // through the same type grammar as the target.
  TypeIdx spec = TypeIdx::invalid();
  std::span<const ItemIdx> methods;
};

// One method signature of a spec declaration: a name with parameter
// and return types, but no body. Implementations supply bodies per
// type and are checked against these under the target.
struct SpecMethod {
  Ident name;
  std::span<const Ident> generic;
  std::span<const ItemFnParam> params;
  TypeIdx return_type = TypeIdx::invalid();
  bool is_unsafe = false;
};

// A named set of method signatures: `spec Iterator<T>`. Method
// signatures are data, not items, so no pass visits them as bodies.
struct ItemSpec {
  Ident name;
  std::span<const Ident> params;
  std::span<const SpecMethod> methods;
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
                                  ItemSpec,
                                  ItemStatic,
                                  ItemConst,
                                  ItemUse>;
  ItemPayload payload;

  std::string_view name() const {
    using I = ItemKind;
    switch (kind) {
      case I::Struct: return payload.get<ItemStruct>().name.name;
      case I::Enum: return payload.get<ItemEnum>().name.name;
      case I::Spec: return payload.get<ItemSpec>().name.name;
      case I::Fn: return payload.get<ItemFn>().name.name;
      case I::Intrinsic: return payload.get<ItemIntrinsic>().name.name;
      case I::Static: return payload.get<ItemStatic>().name.name;
      case I::Const: return payload.get<ItemConst>().name.name;
      case I::Use:
      case I::Impl:
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
//
// Every table and the span area refuse an append they cannot hold, so a
// package larger than the reservation is a diagnostic rather than a
// crash: `exhausted` says the tree being built is incomplete, and the
// parser stops.
struct AstArena {
  // What `spans` is reserved. On identifier-dense code it costs about
  // four bytes per source byte, so the previous mebibyte stopped an input
  // of about a quarter of a megabyte. It reserves address space and commits
  // pages as it hands them out, so a figure no single run reaches costs
  // address space rather than memory.
#if FPAG_BUILD_FLAG(IS_ARCH_64_BITS)
  static constexpr usize DEFAULT_SPAN_CAPACITY = 64ull << 20;
#else
  static constexpr usize DEFAULT_SPAN_CAPACITY = 8ull << 20;
#endif

  // What the node tables are reserved between them. Measured on generated
  // modules, the whole costs about thirteen bytes per source byte - over
  // three times what `spans` costs for the same input - so a node
  // reservation of the same size would bound the input three times sooner
  // than the span reservation does. Each table takes the part of this that
  // its own share below names.
#if FPAG_BUILD_FLAG(IS_ARCH_64_BITS)
  static constexpr usize DEFAULT_NODE_CAPACITY = 512ull << 20;
#else
  static constexpr usize DEFAULT_NODE_CAPACITY = 64ull << 20;
#endif

  // The capacity is a parameter so a caller - a case that has to spend it
  // - can say how much it wants rather than working around the default.
  explicit AstArena(usize span_capacity = DEFAULT_SPAN_CAPACITY) {
    spans.reserve(span_capacity);
  }

  // Whether the reservation is close enough to spent that the next file
  // should not be read. Every table answers for itself, and one of them
  // running out is enough.
  [[nodiscard]] bool nearly_full() const {
    return reservation_nearly_full(spans.capacity(), spans.size()) ||
           exprs.nearly_full() || items.nearly_full() || stmts.nearly_full() ||
           types.nearly_full() || patterns.nearly_full() ||
           blocks.nearly_full() || paths.nearly_full() ||
           literals.nearly_full() || conds.nearly_full();
  }

  // True once any append found its reservation short. What is in the arena
  // is not a tree that passes after it, so a caller refuses the file rather
  // than reading further.
  [[nodiscard]] bool exhausted() const {
    return spans.exhausted() || literals.exhausted() || paths.exhausted() ||
           conds.exhausted() || blocks.exhausted() || types.exhausted() ||
           patterns.exhausted() || exprs.exhausted() || stmts.exhausted() ||
           items.exhausted();
  }

  // How many parsers may append at once, set once before parsing begins.
  // One is exact; more leaves each append room for the appends that may
  // race with it, since the check and the append are not one step.
  void set_parallel_slots(u32 jobs) {
    spans.set_parallel_slots(jobs);
    literals.set_parallel_slots(jobs);
    paths.set_parallel_slots(jobs);
    conds.set_parallel_slots(jobs);
    blocks.set_parallel_slots(jobs);
    types.set_parallel_slots(jobs);
    patterns.set_parallel_slots(jobs);
    exprs.set_parallel_slots(jobs);
    stmts.set_parallel_slots(jobs);
    items.set_parallel_slots(jobs);
  }

  // Both reservations are taken here, and an append into either is atomic, so
  // several parsers can fill one arena at once - which is what reading a
  // package on several threads means. `mem::Arena` would answer the second
  // half of that with corruption rather than with a wrong answer.
  SpanArena spans;

  // Each table's share of the node reservation, in parts per thousand,
  // measured on generated modules: expressions 7.4 bytes per source byte,
  // paths 1.8, items 0.9, types 0.8, patterns 0.8, statements 0.5,
  // literals 0.4, blocks 0.4, conditions 0.02. The shares are the measured
  // ones rounded to whole parts, and a source of a different shape moves
  // them around - which is what the reservation above is sized to absorb.
  NodeVec<Literal, LiteralIdx> literals{DEFAULT_NODE_CAPACITY * 31 / 1000};
  NodeVec<Path, PathIdx> paths{DEFAULT_NODE_CAPACITY * 136 / 1000};
  NodeVec<Cond, CondIdx> conds{DEFAULT_NODE_CAPACITY / 1000};
  NodeVec<Block, BlockIdx> blocks{DEFAULT_NODE_CAPACITY * 28 / 1000};
  NodeVec<TypeNode, TypeIdx> types{DEFAULT_NODE_CAPACITY * 65 / 1000};
  NodeVec<PatternNode, PatternIdx> patterns{DEFAULT_NODE_CAPACITY * 59 / 1000};
  NodeVec<ExprNode, ExprIdx> exprs{DEFAULT_NODE_CAPACITY * 571 / 1000};
  NodeVec<StmtNode, StmtIdx> stmts{DEFAULT_NODE_CAPACITY * 42 / 1000};
  NodeVec<ItemNode, ItemIdx> items{DEFAULT_NODE_CAPACITY * 67 / 1000};
};

}  // namespace ast
