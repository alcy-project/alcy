// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace analyzer {

// Sentinel for "no generic instantiation": table entries recorded
// outside any instantiation-time checking carry this key.
constexpr u32 NO_INST = 0xFFFFFFFFu;

// Sentinel for an inherent method: `MethodInfo::spec` carries the
// index of the implemented spec in the checker's spec table, and
// this for methods from inherent impl blocks.
constexpr u32 NO_SPEC = 0xFFFFFFFFu;

// Per-module type information for type checking and lowering. All TypeIdx
// refer to the package Storage below; all views borrow source bytes.
struct CheckedModule {
  u32 module;

  struct NamedType {
    std::string_view name;
    ir::TypeIdx type;
  };
  std::vector<NamedType> types;
  // Field names parallel to ir::StructType field lists, per struct.
  struct StructInfo {
    ir::TypeIdx type;
    std::vector<std::string_view> fields;
  };
  std::vector<StructInfo> structs;
  // Variant names in declaration order (the index is the discriminant),
  // mirroring structs for matches and construction.
  struct EnumInfo {
    std::string_view name;
    ir::TypeIdx type;
    std::vector<std::string_view> variants;
  };
  std::vector<EnumInfo> enums;
  struct FnSig {
    std::string_view name;
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret;
    // Declaring item for body lowering (invalid for synthesized entries).
    ast::ItemIdx item;
    // Instantiation this signature was checked under; NO_INST for
    // non-generic functions. Lowering keys the callee body by it.
    u32 inst = NO_INST;
    // True for a method's signature. Methods share this table so
    // lowering can name them by index, but a bare call reaches free
    // functions only, whatever order the declarations appear in.
    bool is_method = false;
    // Calling this function is an operation that needs an unsafe
    // block (ADR-0050).
    bool is_unsafe = false;
    // An `extern "C"` declaration (ADR-0051): the name is the symbol
    // the linker resolves, and the body lives outside the program.
    bool is_extern = false;
  };
  // Lazily-instantiated generic methods append signatures during body
  // checking, so element addresses must stay stable: never
  // reallocate-held.
  std::deque<FnSig> functions;
  // Inherent methods per impl block, including associated functions
  // (receiver None). Mirrors the resolved signatures above so call
  // checking can match (self type, name) without re-walking AST.
  // Spec implementations register here too, carrying their spec; the
  // method phase of lookup skips those, and lowering treats both the
  // same, since dispatch is fully static.
  enum class ReceiverKind : u8 { None, ByValue, Shared, Exclusive };
  struct MethodInfo {
    ir::TypeIdx self_type;
    std::string_view name;
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret;
    ReceiverKind receiver;
    ast::ItemIdx item;
    // The type's destructor: `fn drop(self: Self)`. The compiler calls
    // it where the value ends, so it consumes the value and the
    // borrow checker sees that as a move.
    bool is_drop = false;
    // Index of the implemented spec in the checker's spec table;
    // NO_SPEC for methods from inherent impl blocks.
    u32 spec = NO_SPEC;
  };
  // Destructor glue for one type: the `drop` method to call, as an
  // index into `modules[module].methods`. A method reached through a
  // generic impl is the instantiation the checker produced, so its
  // `self_type` already names the concrete type it destroys.
  struct DropGlue {
    u32 module = base::INVALID_IDX;
    u32 index = base::INVALID_IDX;
  };
  // Lazily-instantiated generic methods append during body checking,
  // so element addresses must stay stable: never reallocate-held.
  std::deque<MethodInfo> methods;
  struct StaticInfo {
    std::string_view name;
    ir::TypeIdx type;
    // Initializer for const inlining (invalid for statics in lowering).
    ast::ExprIdx init;
    bool is_const = false;
  };
  std::vector<StaticInfo> statics;
  // Lowering side tables: every checked expression records its type,
  // and every checked call records its callee, so lowering never
  // re-resolves paths or re-derives types. `inst` keys entries
  // checked under a generic instantiation (NO_INST otherwise).
  struct ExprType {
    ast::ExprIdx expr;
    ir::TypeIdx type;
    u32 inst = NO_INST;
  };
  std::vector<ExprType> expr_types;
  struct CallTarget {
    ast::ExprIdx callee;
    bool is_method;
    u32 module;
    u32 index;
    u32 inst = NO_INST;
  };
  std::vector<CallTarget> call_targets;
  // One closure literal: the signature lowering compiles the body
  // against, with the parameters it binds. Keyed by the closure
  // expression; the body stays in the AST. `inst` keys entries
  // checked under a generic instantiation (NO_INST otherwise).
  struct ClosureParam {
    std::string_view name;
    ir::TypeIdx type;
    bool is_mut = false;
    // Where the parameter sits in the closure's signature. Wildcards
    // take a slot without a name, so a named parameter's position in
    // `params` is not its position in the entry block.
    u32 slot = 0;
  };
  struct ClosureLit {
    ast::ExprIdx expr;
    std::vector<ClosureParam> params;
    ir::TypeIdx ret;
    ast::ExprIdx body;
    u32 inst = NO_INST;
    // The declared captures, in list order: the name lowering binds,
    // the mode the list declared, and the outer local's type. Lowering
    // builds the environment from these.
    struct Capture {
      std::string_view name;
      ast::CaptureMode mode = ast::CaptureMode::Move;
      ir::TypeIdx type;
    };
    std::vector<Capture> captures;
  };
  std::vector<ClosureLit> closures;
  // One call through a function value: the callee expression holds
  // the value, and the signature comes from its function type.
  struct IndirectCall {
    ast::ExprIdx callee;
    u32 inst = NO_INST;
  };
  std::vector<IndirectCall> indirect_calls;
  // One named function coerced to a closure value: its code with a
  // null environment. Keyed by the path expression.
  struct ClosureFn {
    ast::ExprIdx expr;
    u32 module;
    u32 index;
    u32 inst = NO_INST;
  };
  std::vector<ClosureFn> closure_fns;
  // Variant resolution for lowering: every checked variant use records
  // its meaning so lowering never re-resolves paths. `variant` is the
  // declaration-order index and doubles as the enum discriminant.
  struct VariantUse {
    ast::PathIdx path;
    ir::TypeIdx enum_type;
    u32 variant = 0;
    u32 inst = NO_INST;
  };
  std::vector<VariantUse> variants;
};

// One instantiation of a generic function or intrinsic: the type
// arguments it bound plus the index of the `FnSig` it registered.
struct FnInstance {
  ast::ItemIdx item;
  // Module the signature registered in; `sig_index` indexes its
  // `functions`.
  u32 module = 0;
  std::vector<ir::TypeIdx> args;
  u32 sig_index = 0;
  // Key into the shared instantiation numbering that keys lowering
  // side tables. Claimed for every function before its body is
  // checked, so a recursive call keys the same context.
  u32 inst = NO_INST;
};

struct CheckedPackage {
  ModuleTree tree;
  // Verified when the checker built them; lowering reseeds its builder
  // from this proof instead of re-verifying.
  ir::VerifiedStorage types;
  // Aligned with tree.modules by index.
  std::vector<CheckedModule> modules;
  // Every generic enum instantiation type, aligned with the
  // checker's instantiation order; indexes key lowering tables.
  std::vector<ir::TypeIdx> generic_insts;
  // Type arguments bound to each generic function or intrinsic, in the
  // same instantiation order as the signature they registered.
  std::vector<FnInstance> fn_insts;
  // Maps a struct/enum field storage copy back to the type it was
  // copied from, as (copy, origin) pairs. Lowering follows these so a
  // field's copied type resolves to its declaring nominal.
  std::vector<std::pair<ir::TypeIdx, ir::TypeIdx>> type_origins;
  // Aligned with `types` by type index: whether ending a value of that
  // type runs code, and which `drop` method to run when the type's own
  // destructor is what runs. Types that only *contain* something
  // destructible have no glue of their own; the destructor of the
  // containing value walks them.
  std::vector<CheckedModule::DropGlue> drop_glue;
  std::vector<bool> needs_drop;
};

// Resolves every type position in the package to interned TypeIdx:
// nominal definitions (structs, enums), signatures, and generic
// instantiations. Reports unknown, duplicate, recursive, and malformed
// types, then checks bodies.
//
// `strings` is the compilation's interner: the storage this returns holds ids
// into it, and lowering and codegen read them back out of it.
base::Result<CheckedPackage, diag::Reported> check_package(
    const ModuleTree& tree,
    ir::PointerWidth width,
    ast::AstArena& ast,
    diag::DiagBag& bag,
    str::StringInterner& strings,
    std::span<const StdHint> std_hints = {},
    debug::Profiler* profiler = nullptr);

}  // namespace analyzer
