// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profiler.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace analyzer {

// Sentinel for "no generic instantiation": table entries recorded
// outside any instantiation-time checking carry this key.
constexpr u32 NO_INST = 0xFFFFFFFFu;

// Sentinel for the definition-site check's instantiation key: a
// generic body checked once against its parameters records under this
// key (ADR-0079), which no concrete check and no lowering reads.
constexpr u32 PARAM_INST = 0xFFFFFFFEu;

// Sentinel for an inherent method: `MethodInfo::spec` carries the
// index of the implemented spec in the checker's spec table, and
// this for methods from inherent impl blocks.
constexpr u32 NO_SPEC = 0xFFFFFFFFu;

// One `comp fn` call an engine answered, kept whole so the other
// engine can be asked the same question (ADR-0054): the declaration,
// the instantiation, the evaluated arguments, and the result.
// `comp_args` are the actuals of the callee's `comp` parameters, the
// ones the specialization key is made of; `args` are every argument,
// which is what the IR engine needs to run the body.
struct CompCallRecord {
  u32 def_module = 0;
  ast::ItemIdx item = ast::ItemIdx::invalid();
  std::vector<ir::TypeIdx> params;
  ir::TypeIdx ret = ir::TypeIdx(base::INVALID_IDX);
  u32 inst = NO_INST;
  std::vector<comp::CompVal> comp_args;
  std::vector<comp::CompVal> args;
  comp::CompVal result;
};

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
    // The declaration the nominal came from; a generic instantiation
    // names its declaration's item.
    ast::ItemIdx item;
    ir::TypeIdx type;
    std::vector<std::string_view> fields;
  };
  std::vector<StructInfo> structs;
  // Variant names in declaration order (the index is the discriminant),
  // mirroring structs for matches and construction.
  struct EnumInfo {
    ast::ItemIdx item;
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
    // A `comp fn`: the call is evaluated during compilation only
    // (ADR-0054).
    bool is_comp = false;
    // An `extern "C"` declaration (ADR-0051): the name is the symbol
    // the linker resolves, and the body lives outside the program.
    bool is_extern = false;
    // A parameter written `args: ..` collects the call's remaining
    // arguments (ADR-0055). Only the compiler-expanded functions carry
    // one, and the slot is not part of `params`.
    bool is_variadic = false;
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
    // Explicit type arguments of a method that declares its own type
    // parameters, in declaration order; empty for every other method.
    // The symbol and the lowered instance key on them.
    std::vector<ir::TypeIdx> method_args;
    // The instantiation key this method's body was checked under: the
    // nominal's instance for a plain method, a fresh number when the
    // method has its own type arguments. Side tables key on it.
    u32 method_inst = NO_INST;
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
    // The declaration this entry describes, so a reference can name
    // the span the source wrote.
    ast::ItemIdx item = ast::ItemIdx::invalid();
    ir::TypeIdx type;
    // Initializer for const inlining (invalid for statics in lowering).
    ast::ExprIdx init;
    bool is_const = false;
    // Module that declares the item. A use from elsewhere checks and
    // evaluates the initializer here, never in the importing scope.
    u32 module = 0;
    // The declaration's `pub`; a static or const of another module is
    // reachable only through it.
    bool is_pub = false;
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
  // Where the last entry recorded for an expression under an
  // instantiation sits. A re-checked expression keeps its last answer
  // (a bare literal is re-checked against its sibling's type), so the
  // index is overwritten rather than first-won.
  std::unordered_map<u64, u32> expr_type_by_key;
  [[nodiscard]] static u64 expr_type_key(ast::ExprIdx expr, u32 inst) {
    return (static_cast<u64>(expr.idx) << 32) | static_cast<u64>(inst);
  }
  struct CallTarget {
    ast::ExprIdx callee;
    bool is_method;
    u32 module;
    u32 index;
    u32 inst = NO_INST;
  };
  std::vector<CallTarget> call_targets;
  // Key of a call target: the callee expression and the instantiation it was
  // recorded under.
  [[nodiscard]] static u64 call_target_key(ast::ExprIdx callee, u32 inst) {
    return (static_cast<u64>(callee.idx) << 32) | static_cast<u64>(inst);
  }
  // Where a call target sits, by that key. Resolving one walked the module's
  // list, and the entry module's list grows with the package: every call it
  // makes is an entry, and lowering each of them walked all of them.
  std::unordered_map<u64, u32> call_target_by_key;
  // One use of a value parameter as an ordinary value: the constant to
  // splice, the annotation type it carries, and the instantiation the
  // body was checked under. Lowering emits the value where the path is.
  struct ValueSplice {
    ast::ExprIdx expr;
    u32 inst = NO_INST;
    u64 value = 0;
    ir::TypeIdx type;
  };
  std::vector<ValueSplice> value_splices;
  // Key of a value splice: the expression and the instantiation it was
  // recorded under, exactly like a call target.
  [[nodiscard]] static u64 value_splice_key(ast::ExprIdx expr, u32 inst) {
    return (static_cast<u64>(expr.idx) << 32) | static_cast<u64>(inst);
  }
  std::unordered_map<u64, u32> value_splice_by_key;
  // The lowering side tables answer through these queries, so the
  // keying and the duplicate rule stay here and no reader scans its
  // own table. An expression's type is the last answer recorded for
  // it; every other table records once per key, and the first entry
  // stands.
  [[nodiscard]] const ExprType* expr_type_of(ast::ExprIdx expr,
                                             u32 inst) const {
    const auto found = expr_type_by_key.find(expr_type_key(expr, inst));
    return found == expr_type_by_key.end() ? nullptr
                                           : &expr_types[found->second];
  }
  [[nodiscard]] const CallTarget* callee_of(ast::ExprIdx callee,
                                            u32 inst) const {
    const auto found = call_target_by_key.find(call_target_key(callee, inst));
    return found == call_target_by_key.end() ? nullptr
                                             : &call_targets[found->second];
  }
  [[nodiscard]] const ValueSplice* value_splice_of(ast::ExprIdx expr,
                                                   u32 inst) const {
    const auto found = value_splice_by_key.find(value_splice_key(expr, inst));
    return found == value_splice_by_key.end() ? nullptr
                                              : &value_splices[found->second];
  }
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
    // The nominal type this literal interned: a one-field struct whose
    // field is the environment tuple. The value owns its captures, and
    // its `Copy`-ness is the environment's (ADR-0064).
    ir::TypeIdx self_type = ir::TypeIdx::invalid();
    // The bare function type the literal coerces to. The view of a
    // nominal borrows the value's environment, so both forms share one
    // signature.
    ir::TypeIdx func_type = ir::TypeIdx::invalid();
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
  // Where the closure literal recorded for an expression under an
  // instantiation sits; a key records once, and the first entry stands.
  std::unordered_map<u64, u32> closure_by_key;
  [[nodiscard]] static u64 closure_key(ast::ExprIdx expr, u32 inst) {
    return (static_cast<u64>(expr.idx) << 32) | static_cast<u64>(inst);
  }
  // One call through a function value: the callee expression holds
  // the value, and the signature comes from its function type.
  struct IndirectCall {
    ast::ExprIdx callee;
    u32 inst = NO_INST;
  };
  std::vector<IndirectCall> indirect_calls;
  // Where the indirect call recorded for a callee under an
  // instantiation sits; a key records once, and the first entry stands.
  std::unordered_map<u64, u32> indirect_call_by_key;
  [[nodiscard]] static u64 indirect_call_key(ast::ExprIdx callee, u32 inst) {
    return (static_cast<u64>(callee.idx) << 32) | static_cast<u64>(inst);
  }
  // One named function coerced to a closure value: its code with a
  // null environment. Keyed by the path expression.
  struct ClosureFn {
    ast::ExprIdx expr;
    u32 module;
    u32 index;
    u32 inst = NO_INST;
  };
  std::vector<ClosureFn> closure_fns;
  // Where the named-function closure recorded for an expression under
  // an instantiation sits; a key records once, and the first entry
  // stands.
  std::unordered_map<u64, u32> closure_fn_by_key;
  [[nodiscard]] static u64 closure_fn_key(ast::ExprIdx expr, u32 inst) {
    return (static_cast<u64>(expr.idx) << 32) | static_cast<u64>(inst);
  }
  [[nodiscard]] const ClosureLit* closure_of(ast::ExprIdx expr,
                                             u32 inst) const {
    const auto found = closure_by_key.find(closure_key(expr, inst));
    return found == closure_by_key.end() ? nullptr : &closures[found->second];
  }
  [[nodiscard]] const ClosureFn* closure_fn_of(ast::ExprIdx expr,
                                               u32 inst) const {
    const auto found = closure_fn_by_key.find(closure_fn_key(expr, inst));
    return found == closure_fn_by_key.end() ? nullptr
                                            : &closure_fns[found->second];
  }
  [[nodiscard]] const IndirectCall* indirect_call_of(ast::ExprIdx callee,
                                                     u32 inst) const {
    const auto found =
        indirect_call_by_key.find(indirect_call_key(callee, inst));
    return found == indirect_call_by_key.end() ? nullptr
                                               : &indirect_calls[found->second];
  }
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
  // One name reference the checker resolved, with the declaration it
  // resolved to, both as source spans. The language server reads these
  // so definition and references answer from the checker's own
  // resolution instead of resolving the name a second time.
  struct Reference {
    diag::Span site;
    diag::Span declaration;
  };
  std::vector<Reference> references;
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
  std::vector<u8> needs_drop;
  // Comp fn calls the checker's evaluator answered, for the IR engine's
  // differential run after lowering built the storage (ADR-0054). The
  // checker's own reader has no lowered IR to offer while it runs.
  std::vector<CompCallRecord> comp_calls;
  // Types that are never moved (ADR-0085): the anchor and anything
  // holding one. Parallel to the type table.
  std::vector<u8> not_movable;
  // One spec declaration, in checker order. A MethodInfo's spec field
  // indexes this list; the LSP reads it to reach the implementations.
  struct SpecDecl {
    std::string_view name;
    ast::ItemIdx item;
  };
  std::vector<SpecDecl> specs;
  // One spec implementation block: the spec it implements and its item.
  struct SpecImplDecl {
    u32 spec = NO_SPEC;
    ast::ItemIdx item = ast::ItemIdx::invalid();
  };
  std::vector<SpecImplDecl> spec_impls;
};

// The static a value path names in `module`: a declaration there, or
// one the module's imports re-export in the value namespace. The
// checker, the lowering, and the comp evaluator ask the same question,
// so the walk lives here rather than once per reader.
const CheckedModule::StaticInfo* find_static(
    std::span<const CheckedModule> modules,
    const ModuleTree& tree,
    u32 module,
    std::string_view name);

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
    ir::SymbolTable& strings,
    std::span<const StdHint> std_hints = {},
    debug::Profiler* profiler = nullptr);

// Helpers the type pass shares between its translation units.

// The key a witness is recorded under: the instantiation's type arguments
// with the witness slot itself left open, so the two sides of a lookup
// agree whichever stand-in the other side used.
std::vector<ir::TypeIdx> witness_key(std::span<const ir::TypeIdx> args,
                                     usize slot);

// Whether a path head is one the module walk owns: the two relative forms
// and the two neighbor-language spellings that get their own message
// (ADR-0060).
bool module_walk_head(std::string_view head);

// Whether a type position mentions any of a declaration's type parameters.
// A spec argument that does is stored as a shape and resolved where the
// target's arguments bind them.
bool type_mentions_params(const ast::AstArena& ast,
                          ast::TypeIdx type,
                          std::span<const ast::GenericParam> params);

// Index of a type parameter in a declaration's parameter list, or the
// parameter count when the name is not a parameter.
u32 param_slot(std::span<const ast::GenericParam> params,
               std::string_view name);

}  // namespace analyzer
