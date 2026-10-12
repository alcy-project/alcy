// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "base/nesting.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/hash/xxh3_hasher.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace analyzer {

struct NominalEntry {
  u32 module;
  std::string_view name;
  ast::ItemIdx item;
  diag::Span span;
  ir::TypeIdx type;
  bool started = false;
  bool complete = false;
};

// A declared capability: `spec Name<params>`. Method signatures live
// on the item; implementations register their methods as ordinary
// `MethodInfo` entries carrying this index.
struct SpecEntry {
  u32 module;
  std::string_view name;
  ast::ItemIdx item;
  diag::Span span;
};

// The target of one `impl S for T`: a nominal with one argument
// descriptor per parameter, or one scalar primitive. A descriptor is
// either a concrete type or the name of the impl parameter standing in
// that position; nested shapes never reach the table, so overlap stays
// decidable. A primitive target carries no arguments and is never
// generic; the two forms are mutually exclusive.
struct SpecTarget {
  u32 nominal = 0;
  struct Arg {
    bool is_param = false;
    std::string_view param;
    ir::TypeIdx type = ir::TypeIdx::invalid();
    // A spec argument that mentions an implementation parameter without
    // being the parameter itself (`&T`, `Option<T>`): the AST shape is
    // resolved where the target's arguments bind them.
    ast::TypeIdx shape = ast::TypeIdx::invalid();
    // A target argument that names a value parameter (`[T; N]`): the
    // receiver's const argument is what binds it.
    bool is_value = false;
    ast::TypeIdx annotation = ast::TypeIdx::invalid();
  };
  std::vector<Arg> args;
  bool is_primitive = false;
  ir::TypeIdx primitive = ir::TypeIdx::invalid();
};

// One coherence record: spec `spec` implemented for `target` by
// `item` in `module`. At most one record per spec overlaps any
// concrete type. Generic records persist the spec-argument shapes
// beside the target so call-site instantiation resolves both.
struct SpecImplEntry {
  u32 spec = 0;
  u32 module = 0;
  SpecTarget target;
  std::vector<SpecTarget::Arg> spec_args;
  ast::ItemIdx item = ast::ItemIdx::invalid();
  diag::Span span;
};

// One instantiation of a generic enum: the checked shape of
// `Nominal<args>`, interned once and shared by identity.
struct GenericInstance {
  u32 nominal = 0;
  std::vector<ir::TypeIdx> args;
  ir::TypeIdx type;
  bool started = false;
  bool complete = false;
};

class Checker {
 public:
  Checker(const ModuleTree& tree,
          ir::PointerWidth width,
          ast::AstArena& ast,
          diag::DiagBag& bag,
          ir::SymbolTable& strings,
          std::span<const StdHint> std_hints = {},
          debug::Profiler* profiler = nullptr);

  const ModuleTree& tree;
  ast::AstArena& ast;
  ir::PointerWidth width;
  diag::DiagBag& bag;
  std::span<const StdHint> std_hints;
  // Where the trace events go, or nothing. The type passes run module
  // by module - nominals, signatures, bodies, drops - and each sweep is
  // wrapped where it is driven, in `check_package`.
  debug::Profiler* profiler = nullptr;
  ir::StorageBuilder builder;
  // The compilation's one interner; a second one here would mint storage ids
  // that lowering and codegen could not resolve.
  ir::SymbolTable& interner;
  std::vector<NominalEntry> nominals;
  std::vector<SpecEntry> specs;
  // Where a module's entries sit in those two tables. Both tables span the
  // whole package, so a lookup that walked one charged a module for every
  // type in it; these narrow the walk to the module that asked. Positions,
  // not pointers, because the tables grow while they are being filled.
  std::vector<std::vector<u32>> nominals_of_module;
  std::vector<std::vector<u32>> specs_of_module;
  // Every spec implementation in the tree, in registration order.
  // Coherence is checked against this as each impl registers.
  std::vector<SpecImplEntry> spec_impls;
  // Inherent impls whose target is a compound shape (`impl<T> [T]`):
  // there is no nominal to index them by, so each block's shape is
  // matched against a receiver at lookup.
  struct CompoundImplEntry {
    u32 module = 0;
    ast::ItemIdx item = ast::ItemIdx::invalid();
  };
  std::vector<CompoundImplEntry> compound_impls_;
  std::vector<GenericInstance> generic_instances;
  // The same instances, by the type each minted and by the nominal each
  // instantiates. Both questions were answered by walking every instance the
  // package had: which instance a type is, once per lookup, and which instance
  // a request already has, once per request.
  std::unordered_map<u32, u32> instance_by_type_;
  std::vector<std::vector<u32>> instances_by_nominal_;
  std::vector<FnInstance> fn_instances;
  // Shared instantiation numbering that keys lowering side tables.
  // Holds each generic enum/struct instance type in creation order and
  // an invalid placeholder per generic function body, so both kinds
  // draw keys from one space.
  std::vector<ir::TypeIdx> inst_numbering;
  // The numbering a type was given, by the type. A lookup walked the whole
  // numbering, and a package that instantiates a type per module asks for a
  // method's instantiation once per method it instantiates.
  std::unordered_map<u32, u32> inst_by_type_;
  // `ref_type` appends a storage copy of a type so a struct's field
  // range stays contiguous. This maps each copy back to the type it
  // was copied from, so owner lookups accept both indexes.
  std::vector<std::pair<ir::TypeIdx, ir::TypeIdx>> type_origins_;
  // type_origin resolves a chain of those, and the type checker asks for one
  // at the head of every structural comparison, so a lookup that walked the
  // list charged the checker its types times its copies. This answers the
  // same question: a later entry for a copy wins, as walking from the back
  // did.
  std::unordered_map<u32, u32> type_origins_by_index_;
  // Const integer values the checker resolved for type positions, by
  // the initializer they came from; the in-progress set catches a
  // const that reaches itself, and `const_init_checked_` keeps one
  // initializer from being checked twice.
  std::unordered_map<u32, u64> const_int_values_;
  std::unordered_set<u32> const_init_in_progress_;
  std::unordered_set<u32> const_init_checked_;
  // Initializers whose check reported an error. A later request sees
  // the failure rather than evaluating an error-typed initializer and
  // reporting a follow-on.
  std::unordered_set<u32> const_init_failed_;
  // Functions whose bodies the const pre-pass already checked, so the
  // bodies sweep does not check them a second time.
  std::unordered_set<u32> prechecked_fns_;
  // Items whose declaration has registered, by item index. On-demand
  // resolution consults it so a declaration that reaches itself stops
  // instead of recursing, and so a later sweep does not register the
  // same declaration twice.
  std::unordered_set<u32> registered_items_;
  // Nonzero while a body is being checked by the const pre-pass.
  // A call reached there types its callee's body too, so the evaluator
  // finds the whole call graph typed.
  u32 prechecking_fn_ = 0;
  // `MaybeUninit<T>` wrappers interned so far, as (wrapper, payload).
  std::vector<std::pair<ir::TypeIdx, ir::TypeIdx>> uninit_types_;
  // Destructor resolution, aligned with the type table by index.
  std::vector<CheckedModule::DropGlue> drop_glue_;
  std::vector<u8> needs_drop_;
  // Comp fn calls the const pre-pass answered, carried on the checked
  // package so lowering can compare them against the IR engine once the
  // storage exists (ADR-0054).
  std::vector<CompCallRecord> comp_calls_;
  // Interned name every `MaybeUninit` wrapper carries.
  str::StringPoolId uninit_name_id = str::INVALID_STRING_POOL_ID;
  // Set once the shared table has no room for another name; the sweep
  // that saw it stops, and `check_package` refuses the package.
  bool name_table_exhausted_ = false;
  // Active type-parameter scope: innermost last. Pushed while
  // instantiating a generic enum or checking its members.
  std::vector<std::pair<std::string_view, ir::TypeIdx>> type_params;
  // One active value parameter: the value it bound, the module and
  // annotation a value use resolves its type through.
  struct ValueParam {
    std::string_view name;
    u64 value = 0;
    u32 module = NO_MODULE;
    ast::TypeIdx annotation = ast::TypeIdx::invalid();
  };
  // Active value-parameter scope: the parameters a declaration
  // annotated (`N: usize`) bind their compile-time value here while
  // the declaration's members resolve.
  std::vector<ValueParam> value_params;
  // Index into generic_instances while checking an instantiated
  // method body (NO_INST otherwise); keys the lowering side tables.
  u32 cur_inst = NO_INST;
  // The spec whose implementation is being checked, innermost last.
  // A method body inside `impl S for T` resolves sibling calls
  // through S itself, which is in scope there by construction.
  std::vector<u32> spec_scope;
  std::vector<CheckedModule> modules;
  std::vector<u32> parents;
  // Inherent methods declared so far, keyed by the nominal they attach
  // to. Spec methods are not recorded: an inherent method shadows a
  // spec method of the same name by design. The set answers whether one is
  // already recorded, which a walk over every declaration so far asked by
  // comparing the names of all of them.
  struct InherentMethod {
    u32 target_module = NO_MODULE;
    std::string_view target_name;
    std::string_view method;
    bool operator==(const InherentMethod&) const = default;
  };
  struct InherentMethodHash {
    usize operator()(const InherentMethod& key) const {
      const hash::Xxh3Hasher64 hash;
      return hash(key.target_name) * 31u + hash(key.method) + key.target_module;
    }
  };
  std::unordered_set<InherentMethod, InherentMethodHash> inherent_methods_;

  // Body-checking state, reset per function.
  struct Local {
    std::string_view name;
    ir::TypeIdx type;
    bool is_mut;
    bool comp_known = false;
    // Index into the innermost closure's capture list when this local
    // is a capture binding, or NO_CAPTURE otherwise.
    u32 capture = NO_CAPTURE;
  };
  // The capture index that names no capture binding.
  static constexpr u32 NO_CAPTURE = 0xFFFFFFFFu;
  bool in_fn = false;

  std::vector<std::vector<Local>> scopes;
  // Nonzero inside an `unsafe { ... }` block: the operations the gate
  // covers may appear there (ADR-0050). A closure literal written
  // inside the block inherits it, because the rule is lexical.
  u32 unsafe_depth = 0;
  // One declared capture while its closure's body is checked: the
  // name as desugared, the mode the list declared, the outer local's
  // type, and whether the body used it. A use resolving below the
  // innermost boundary was not captured at all.
  struct CaptureEntry {
    std::string_view name;
    ast::CaptureMode mode = ast::CaptureMode::Move;
    ir::TypeIdx type;
    diag::Span span;
    bool used = false;
  };
  struct ClosureBound {
    usize scope = 0;
    std::vector<CaptureEntry> captures;
  };
  std::vector<ClosureBound> closure_bounds;
  u32 loop_depth = 0;
  // Nonzero while checking comp evaluation contexts (comp block
  // contents and comp declaration initializers).
  u32 comp_depth = 0;
  // Verifies comp-known-ness while block scopes are still alive
  // (comp blocks and their nested bodies).
  bool verify_comp_known = false;
  ir::TypeIdx fn_ret = ir::TypeIdx(0);
  void register_nominals();
  // Every annotated generic parameter names the type of a compile-time
  // value, so the annotation must resolve to an integer.
  void validate_value_parameter_types();
  // Only a compiler-expanded function may declare `args: ..`
  // (ADR-0055); the expansion is what gives the slot meaning.
  void validate_variadic_parameters();
  bool is_expanded_variadic(ast::ItemIdx item) const;
  // The recorder the const pre-pass's evaluator reports through; the
  // records ride the checked package to lowering (ADR-0054).
  static void record_comp_call(void* context, const CompCallRecord& record);
  NominalEntry* find_nominal(u32 module, std::string_view name);
  // A spec declaration by module and name; at most one per module.
  SpecEntry* find_spec(u32 module, std::string_view name);
  // The package `module` belongs to, by the identity a `use` spells:
  // the root package's name, a dependency's, or a staged member's.
  std::string_view package_of(u32 module) const;
  // The spec policy of `package`, or nullptr when the tree carries
  // none, which reads as every spec being open.
  const PackagePolicy* policy_of(std::string_view package) const;
  // A spec visible from `module`: declared there, imported, or
  // injected from a prelude facade. Mirrors the type-namespace walk.
  SpecEntry* find_spec_in_scope(u32 module, std::string_view name);
  // Whether `spec` answers method calls from `module`: declared
  // there, imported under any local name, or injected from a facade.
  bool spec_in_scope(u32 module, const SpecEntry& spec);
  // Registers one `spec` item: rejects a colliding type or spec name
  // in the same module, duplicate methods, and method-level type
  // parameters, which arrive with bounds.
  void register_spec(u32 module, ast::ItemIdx item);
  // Verifies a staged operator spec against the compiler's canonical
  // table (ADR-0053); true for every other spec.
  bool check_operator_spec(const ast::ItemSpec& declaration);
  // Registers one `impl S for T`: resolves the spec and the target,
  // enforces coherence, and checks the methods against the declared
  // signatures. Generic targets defer bodies to call-site
  // instantiation, exactly like generic inherent impls.
  void register_spec_impl(u32 module, ast::ItemIdx item);
  // Records an inherent method declaration, answering false when the
  // nominal already carries one under this name. Impl blocks may repeat
  // a name in one module or in another, and the second method would be
  // the one a receiver shape reaches while the first sits unreachable.
  bool record_inherent_method(u32 target_module,
                              std::string_view target_name,
                              std::string_view method);
  // Declared signature of spec method `name` under `self_type`, with
  // the spec's parameters bound to `spec_args`. False when the spec
  // declares no such method.
  bool spec_method_sig(u32 spec,
                       std::string_view name,
                       std::span<const ir::TypeIdx> spec_args,
                       ir::TypeIdx self_type,
                       u32 module,
                       std::vector<ir::TypeIdx>& params_out,
                       ir::TypeIdx& ret_out,
                       CheckedModule::ReceiverKind& receiver_out);
  // Whether two spec targets overlap: same nominal with arguments
  // pairwise equal or a bare impl parameter. Concrete types never
  // overlap each other, so coherence needs no solver.
  bool spec_targets_overlap(const SpecTarget& a, const SpecTarget& b);
  // Whether two impl records carry the same target: the same nominal
  // with alpha-equivalent arguments. The super-spec requirement needs
  // the identical shape, which overlap is too weak to state.
  bool spec_target_shapes_match(const SpecImplEntry& a, const SpecImplEntry& b);
  // Resolves every declared super-spec, rejects cycles, and requires an
  // implementation of each ancestor beside every implementation of a
  // spec (ADR-0053). Runs after every module's signatures registered,
  // so a super declared later still resolves.
  void check_superspecs();
  // Synthesizes one spec method entry for a generic instantiation
  // and checks its body under the substitution, mirroring
  // `instantiate_method` with the declared signature as the check.
  const CheckedModule::MethodInfo* instantiate_spec_method(
      u32 impl_module,
      u32 spec,
      std::span<const ir::TypeIdx> spec_args,
      ir::TypeIdx self_type,
      const std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
      std::span<const ValueParam> value_scope,
      const ast::ItemImpl& impl,
      std::string_view name);
  // Checks the bodies of a concrete `impl S for T` under the target.
  // The record carries the resolved arguments, so no path is
  // re-resolved and a rejected impl simply has no record to find.
  void check_spec_impl_bodies(u32 module, ast::ItemIdx item);
  // A declaration recognized by its reserved name (`Range`, `Bound`).
  NominalEntry* builtin_nominal(std::string_view name);
  u32 find_child_module(u32 module, std::string_view name) const;
  ir::TypeIdx primitive_type(ast::PrimitiveKind kind, diag::Span span);
  ir::TypeIdx error_type();
  CheckedModule::ReceiverKind classify_receiver(ir::TypeIdx first,
                                                ir::TypeIdx self);
  // Interns a name, or reports the shared table as spent once and returns
  // an invalid id. A caller that gets one returns the error type; no id
  // from a spent table reaches storage.
  str::StringPoolId intern_name(std::string_view name);
  ir::TypeIdx intern_nominal(NominalEntry& entry);
  // `MaybeUninit<T>` wrapper for `payload`, interned per payload.
  ir::TypeIdx intern_uninit(ir::TypeIdx payload);
  // Payload of a `MaybeUninit<T>` wrapper; invalid for any other type.
  ir::TypeIdx uninit_payload(ir::TypeIdx type) const;
  // Key of a type in the shared instantiation numbering, matching what
  // lowering side tables use; NO_INST for a non-instantiation.
  u32 inst_index(ir::TypeIdx type) const;
  // Declared type parameters of a function or method item.
  std::span<const ast::GenericParam> fn_generic_params(ast::ItemIdx item) const;
  // Item name for diagnostics.
  std::string_view fn_name(ast::ItemIdx item) const;
  // Whether calling the function or intrinsic item is an operation
  // that needs an unsafe block.
  bool fn_is_unsafe(ast::ItemIdx item) const;
  // Whether the function item carries the `comp fn` marker (ADR-0054).
  bool fn_is_comp(ast::ItemIdx item) const;
  // A type parameter a declared parameter type pins on its own. The
  // flags say where in the argument the bound type sits: `through_ref`
  // takes a reference's pointee, `through_raw` takes a raw pointer's,
  // and `through_uninit` then unwraps the `MaybeUninit` wrapper, which
  // is how a generic intrinsic recovers `T` from a
  // `&mut MaybeUninit<T>` parameter.
  struct DeclaredBinding {
    u32 slot = 0;
    bool through_ref = false;
    bool through_uninit = false;
    bool through_slice = false;
    bool through_raw = false;
  };
  DeclaredBinding declared_binding(std::span<const ast::GenericParam> params,
                                   const ast::TypeNode& declared) const;
  // Declared parameters of a function or intrinsic item.
  std::span<const ast::ItemFnParam> fn_params(ast::ItemIdx item) const;
  // Declared return type of a function or intrinsic item; invalid when
  // the item declares none, which means `()`.
  ast::TypeIdx fn_return_type(ast::ItemIdx item) const;
  // Binds a generic free function's type parameters from explicit
  // turbofish arguments or from arguments whose declared type is a
  // parameter, then instantiates. Returns null after diagnosing.
  const CheckedModule::FnSig* resolve_generic_fn(
      u32 module,
      ast::ItemIdx item,
      const std::span<const ast::ExprIdx>& args,
      const std::span<const ast::TypeIdx>& explicit_args,
      diag::Span span);
  const CheckedModule::FnSig* instantiate_fn(
      u32 module,
      ast::ItemIdx item,
      const std::vector<ir::TypeIdx>& args);
  ir::TypeIdx instantiate_generic(u32 nominal,
                                  const std::vector<ir::TypeIdx>& args,
                                  diag::Span span);
  const GenericInstance* generic_find(ir::TypeIdx idx) const;
  u32 nominal_index(const NominalEntry* entry);
  // Instantiation of `nominal` matching `type`, or null. Callers push
  // its substitution while resolving member types, then pop.
  const GenericInstance* generic_instance_for(u32 nominal,
                                              ir::TypeIdx type) const;
  // Watermarks of both generic scopes, so a pop restores whatever a
  // push added without knowing which slots the declaration kinds bound.
  struct GenericScope {
    usize type_params = 0;
    usize value_params = 0;
  };
  GenericScope push_generic_scope(const GenericInstance& instance);
  void pop_generic_scope(const GenericScope& kept);
  // The value parameter a visible name is bound to, or null. The
  // innermost binding wins.
  const ValueParam* value_param(std::string_view name) const;
  // The u64 a const type argument carries.
  u64 const_type_value(ir::TypeIdx type) const;
  // Parameter names of a nominal declaration, whether struct or enum.
  std::span<const ast::GenericParam> nominal_params(const NominalEntry& entry);
  // Appends a storage copy of `type` and records its origin.
  ir::TypeIdx storage_copy(ir::TypeIdx type);
  // Follows storage copies back to the type they were made from.
  ir::TypeIdx type_origin(ir::TypeIdx type) const;
  // Interns a nominal's type. Generic declarations defer: their type
  // is fixed later against the expectation or the scrutinee, so the
  // error type marks "resolve from context".
  ir::TypeIdx nominal_owner_type(NominalEntry* entry);
  bool walk_module_prefix(u32 module,
                          ast::PathIdx path,
                          std::string_view what,
                          u32& module_out);
  bool resolve_type_path(u32 module,
                         ast::PathIdx path,
                         u32& module_out,
                         std::string_view& name_out);
  ir::TypeIdx resolve_type(u32 module,
                           ast::TypeIdx type,
                           const ir::TypeIdx* self,
                           bool behind_ref = false);
  // Resolves one argument at a value-declared generic slot: an integer
  // literal, a visible value parameter, or a `const` item. `span` is
  // where a failure is reported and `param` names the slot.
  bool resolve_value_argument(u32 module,
                              ast::TypeIdx arg,
                              std::string_view param,
                              diag::Span span,
                              u64& out);
  static bool is_known_intrinsic(std::string_view name);
  bool check_intrinsic_signature(u32 module,
                                 const ast::ItemIntrinsic& intrinsic,
                                 const std::vector<ir::TypeIdx>& params,
                                 ir::TypeIdx ret);
  void process_module(u32 module);
  bool has_value_cycle(ir::TypeIdx root,
                       std::vector<ir::TypeIdx>& stack,
                       const ir::Storage& storage);
  std::string_view nominal_name(ir::TypeIdx idx) const;
  ir::TypeTag tag_of(ir::TypeIdx idx) const;
  bool is_integer_tag(ir::TypeTag tag) const;
  bool is_float_tag(ir::TypeTag tag) const;
  bool is_void(ir::TypeIdx idx) const;
  bool is_never(ir::TypeIdx idx) const;
  bool is_error(ir::TypeIdx idx) const;
  static const char* pretty_tag(ir::TypeTag tag);
  bool types_equal(ir::TypeIdx a, ir::TypeIdx b);
  bool types_equal_inner(ir::TypeIdx a, ir::TypeIdx b, std::vector<u64>& seen);
  bool coerces_to_shared(ir::TypeIdx expected, ir::TypeIdx actual);
  bool coerces_array_to_slice(ir::TypeIdx expected, ir::TypeIdx actual);
  ir::TypeIdx unify(ir::TypeIdx expected,
                    ir::TypeIdx actual,
                    diag::Span span,
                    std::string_view what);
  const Local* lookup_local(std::string_view name) const;
  // The scope level binding `name`, or one past the last scope when
  // none does. Locals shadow outward, so this is the level a use
  // resolves at.
  usize scope_of(std::string_view name) const;
  bool classify_suffix(std::string_view spelling,
                       ir::TypeTag& tag,
                       bool& is_float,
                       diag::Span span);
  ir::TypeIdx check_literal(ast::LiteralIdx value, const ir::TypeIdx* expected);
  void validate_cycles(const ir::Storage& storage);
  const CheckedModule::StaticInfo* lookup_static(u32 module,
                                                 std::string_view name);
  // The registered-only search: `lookup_static` registers a declaration
  // the signature sweep has not reached yet, while a const method can
  // only read what is already there.
  const CheckedModule::StaticInfo* find_static(u32 module,
                                               std::string_view name) const;
  // Registers the signature of a non-generic function declared in
  // `module` under `name`, when a use reaches it before the signature
  // sweep has. True when a declaration exists; an already-registered
  // signature is left alone, so processing the module later finds it.
  bool ensure_function_named(u32 module, std::string_view name);
  // The same for a const or static item, so an array length can name
  // one declared later in the module.
  bool ensure_static_named(u32 module, std::string_view name);
  // Registers one non-generic function signature, once.
  void register_fn_signature(u32 module, ast::ItemIdx item);
  // Registers one const or static declaration, once.
  void register_static(u32 module, ast::ItemIdx item);
  // An array length named by a path: the const item's initializer is
  // evaluated at compile time, through the comp evaluator, once the
  // initializers of the consts it references are checked.
  bool const_array_length(u32 module,
                          const ast::TypeArray& array,
                          diag::Span span,
                          u64& out);
  // The integer value of a const item, evaluated through the comp
  // evaluator; `span` is where a failure is reported, and the key
  // parameter is the wording for a value that is not a non-negative
  // integer.
  template <i18n::Key not_const_key>
  bool const_int_value(const CheckedModule::StaticInfo& info,
                       diag::Span span,
                       u64& out);
  // Ensures the const item's initializer is checked (and its
  // dependencies', transitively), so the evaluator finds the
  // expressions it reads typed. A const that reaches itself is a
  // cycle.
  bool ensure_const_checked(const CheckedModule::StaticInfo& info);
  bool check_const_dependencies(u32 module, ast::ExprIdx expr);
  bool check_const_dependencies_block(u32 module, ast::BlockIdx block);
  bool check_const_dependency_path(u32 module, ast::PathIdx path);
  // Checks the body of a non-generic function a const initializer
  // calls, because the evaluator interprets it before the bodies pass
  // would have typed its expressions.
  void precheck_called_fn(u32 module, ast::ExprIdx callee);
  // Types the body of one resolved free function under its own
  // instantiation. A call inside a body being prechecked reaches the
  // same treatment, so the whole call graph the evaluator will walk is
  // typed by the time it runs.
  void precheck_fn_body(const CheckedModule::FnSig& fn);
  // The same for a method a const initializer calls: its body is typed
  // under the receiver's declared self type so the evaluator can
  // interpret it. Methods of generic impls were typed at instantiation
  // and are left alone.
  void precheck_called_method(const CheckedModule::MethodInfo& method);
  const CheckedModule::FnSig* lookup_function(u32 module,
                                              std::string_view name);
  // Generic free function item in scope, by name; invalid if absent.
  ast::ItemIdx lookup_generic_fn(u32 module, std::string_view name);

  struct VariantMatch {
    NominalEntry* enom = nullptr;
    u32 variant = 0;
  };
  bool find_variant_in(u32 module,
                       std::string_view name,
                       std::vector<VariantMatch>& out);
  bool find_variant(u32 module,
                    std::string_view name,
                    diag::Span span,
                    VariantMatch& out);
  NominalEntry* find_nominal_in_scope(u32 module, std::string_view name);
  // Whether a declaration of `declaring` is reachable from `from`: its
  // own module always, every other module only when it is `pub`. A
  // module path that crossed a package boundary was already trimmed to
  // the dependency's export list, so this is the whole rule.
  static bool declaration_visible(u32 from, u32 declaring, bool is_pub) {
    return from == NO_MODULE || from == declaring || is_pub;
  }
  // The same, reporting a private reach at `span` and answering whether
  // the use may proceed.
  bool check_item_access(u32 from,
                         u32 declaring,
                         bool is_pub,
                         diag::Span span,
                         std::string_view name);
  // A field's marker, checked the same way; `owner` names the struct in
  // the report.
  bool check_field_access(u32 from,
                          u32 declaring,
                          bool is_pub,
                          diag::Span span,
                          std::string_view field,
                          std::string_view owner);
  // Finds an inherent method, instantiating generic impls on demand
  // (which checks the method body under the substitution). `module` is
  // the module the call is written in; a method of another module
  // needs its `pub`.
  const CheckedModule::MethodInfo* lookup_inherent_method(ir::TypeIdx self,
                                                          std::string_view name,
                                                          u32 module,
                                                          diag::Span span);
  // Whether a compound impl target (`[T]`, `[T; N]`) matches a receiver,
  // binding the implementation's parameters to the receiver's parts.
  bool match_compound_target(
      u32 module,
      ast::TypeIdx target,
      std::span<const ast::GenericParam> params,
      ir::TypeIdx self,
      std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
      std::vector<ValueParam>& value_scope);
  // Collects the generic impls of the package, by the nominal each targets.
  // Built on the first generic lookup and rebuilt if a nominal was interned
  // since, because a lookup may come before the last one is.
  void index_generic_impls();
  // Collects the inherent methods that declare their own type parameters,
  // by the nominal their impl targets. Built and rebuilt like the generic
  // impls index.
  void index_generic_methods();
  // Finds a method for `self`: an inherent one, or with `spec_only` a
  // spec implementation reachable from `module`. Only in-scope specs
  // match.
  const CheckedModule::MethodInfo* lookup_method(ir::TypeIdx self,
                                                 std::string_view name,
                                                 u32 module,
                                                 diag::Span span,
                                                 bool spec_only,
                                                 u32 spec_filter = U32_MAX);
  // The compiler-owned operator spec `name` declared in staged core, or
  // U32_MAX when the standard library is not staged or declares none.
  u32 operator_spec(std::string_view name) const;
  // The nominal an index receiver names, looking through one reference,
  // or an error type when the receiver is no nominal.
  ir::TypeIdx index_nominal(ir::TypeIdx receiver);
  const CheckedModule::MethodInfo* instantiate_method(
      u32 impl_module,
      ir::TypeIdx self_type,
      const std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
      std::span<const ValueParam> value_scope,
      const ast::ItemImpl& impl,
      std::string_view name,
      std::span<const ir::TypeIdx> method_args = {});
  // Finds an inherent method that declares its own type parameters for
  // `self` and instantiates it with the call's explicit arguments; null
  // when no such method carries the name.
  const CheckedModule::MethodInfo* instantiate_generic_method(
      u32 module,
      ir::TypeIdx self,
      std::string_view name,
      std::span<const ast::TypeIdx> type_args,
      diag::Span span);
  void record_call(u32 module,
                   ast::ExprIdx callee,
                   const CheckedModule::FnSig* fn);
  void record_call(u32 module,
                   ast::ExprIdx callee,
                   const CheckedModule::MethodInfo* method);
  // Recognizes a `drop` method and rejects a signature that could not
  // act as a destructor. Returns whether the method is one.
  bool check_drop_signature(u32 module,
                            ir::TypeIdx self_type,
                            const CheckedModule::FnSig& sig,
                            CheckedModule::ReceiverKind receiver,
                            ast::ItemIdx item);
  // Resolves the destructor of every type in the package, instantiating
  // generic ones, and records which types run code when a value ends.
  void resolve_drops();
  bool drop_scan(ir::TypeIdx type, std::vector<u32>& stack);
  bool holds_destructible(ir::TypeIdx type, std::vector<u32>& stack);
  CheckedModule::DropGlue find_drop_glue(ir::TypeIdx type);
  // Destructors by the type they drop. Resolving a drop asks this of every
  // type the package holds, and answering by walking every method in the
  // package charged a type for the whole package. Filled as methods register,
  // which is also when a generic instantiation adds one, so it cannot fall
  // behind the table it indexes.
  std::unordered_map<u32, CheckedModule::DropGlue> drop_glue_by_type_;
  // Methods by the type they are declared on, as (module, position). A call
  // is resolved by walking the methods of the type it names, not every
  // method in the package.
  std::unordered_map<u32, std::vector<std::pair<u32, u32>>> methods_by_self_;
  // The nominal that declares a type, by the type it built. A field access
  // needs the declaration that names a field, a match on an enumeration needs
  // the one that names its variants, a method call needs the one its receiver
  // belongs to, and a diagnostic needs the one to call the type by. Each asked
  // by walking every nominal in the package, so one question cost the package.
  std::unordered_map<u32, u32> nominal_by_type_;
  // The generic impls of the package, by the nominal they target, and the
  // nominal count the index was built against.
  std::vector<std::vector<std::pair<u32, ast::ItemIdx>>>
      generic_impls_by_nominal_;
  // Inherent methods that declare their own type parameters, by the nominal
  // their impl targets: `(module, impl item, method item)`. Such a method
  // is instantiated at its call site, which spells the arguments.
  struct GenericMethodEntry {
    u32 module = 0;
    ast::ItemIdx impl_item = ast::ItemIdx::invalid();
    ast::ItemIdx method_item = ast::ItemIdx::invalid();
  };
  std::vector<std::vector<GenericMethodEntry>> generic_methods_by_nominal_;
  bool generic_methods_indexed_ = false;
  usize indexed_nominals_ = 0;
  bool generic_impls_indexed_ = false;
  // Where a module's children are, by the name a path calls them, and the
  // module each package root opens, by the identity a path spells. A path of
  // two segments or more walked every module in the package to find the one
  // whose tail matched, and named a package by walking the roots.
  std::vector<std::unordered_map<std::string_view, u32>> children_by_tail_;
  std::unordered_map<std::string_view, u32> root_by_identity_;
  // An address as a key. The default hash is the address itself, whose low
  // bits carry the alignment every such key shares, so a table with a
  // power-of-two bucket count puts them in every k-th bucket. Mixing the bits
  // spreads them.
  struct AddressHash {
    usize operator()(const void* address) const {
      usize bits = reinterpret_cast<usize>(address);
      bits >>= 4;
      bits ^= bits >> 15;
      bits *= 0x9e3779b1u;
      bits ^= bits >> 13;
      return bits;
    }
  };
  // Where a signature and a method sit in their module, by address. Both
  // tables are deques, so an address a caller holds stays the one it holds,
  // and resolving a call site otherwise walked every function in the package
  // to turn its pointer back into a position.
  std::unordered_map<const void*, std::pair<u32, u32>, AddressHash>
      position_by_address_;
  // Registers a function and answers the slot it went into, which is what a
  // call site holds and what the positions above are keyed by.
  CheckedModule::FnSig& add_function(u32 module, CheckedModule::FnSig sig);
  // Registers a method and returns it. Where two modules declare a
  // destructor for one type, the earlier module's is kept, which is what a
  // walk in module order would have found.
  CheckedModule::MethodInfo& add_method(u32 module,
                                        CheckedModule::MethodInfo info);

  struct PathValue {
    enum class Kind : u8 {
      Local,
      ValueParam,
      Static,
      Function,
      AssocFunction,
      GenericFn,
      UnitVariant,
      TupleVariant,
      Type,
    };
    Kind kind = Kind::Type;
    ir::TypeIdx type = ir::TypeIdx(0);
    const CheckedModule::FnSig* function = nullptr;
    const CheckedModule::MethodInfo* method = nullptr;
    NominalEntry* enom = nullptr;
    ast::ItemIdx generic_item = ast::ItemIdx::invalid();
    u32 variant = 0;
  };
  // `type_args` carries a turbofish from the enclosing expression path.
  // `expr` is the expression the path was written at; a value-parameter
  // resolution records its splice for lowering under that expression.
  bool resolve_value_path(u32 module,
                          ast::ExprIdx expr,
                          ast::PathIdx path,
                          std::span<const ast::TypeIdx> type_args,
                          PathValue& out);
  bool resolve_variant_path(u32 module, ast::PathIdx path, PathValue& out);
  std::vector<ir::TypeIdx> variant_payloads(const PathValue& resolved,
                                            ir::TypeIdx enum_type,
                                            diag::Span span);
  // Binds type parameters from constructor arguments when a field's
  // declared type is exactly a parameter. Returns null when any
  // parameter stays unbound.
  const GenericInstance* infer_from_payload_args(
      u32 module,
      u32 nominal,
      const PathValue& resolved,
      std::span<const ast::ExprIdx> args);
  NominalEntry* resolve_struct_path(u32 module, ast::PathIdx path);
  void collect_pattern_idents(ast::PatternIdx pattern,
                              std::vector<std::string_view>& out);
  void bind_error_idents(u32 module, ast::PatternIdx pattern);

  // Binds a pattern against a type, declaring locals. Returns true
  // when the pattern is refutable (declarations reject those).
  // `bind_comp_known` marks the declared locals comp-known for `comp`
  // parameters and declarations; anything bound while checking comp
  // evaluation contexts counts as comp-known as well.
  bool bind_comp_known = false;
  bool bind_pattern(u32 module, ast::PatternIdx pattern, ir::TypeIdx type);
  bool is_bare_int_literal(ast::ExprIdx expr) const;
  bool expr_comp_known(u32 module, ast::ExprIdx expr) const;
  bool expr_comp_known_block(u32 module, ast::BlockIdx block) const;
  ir::TypeIdx check_binary_operands(u32 module,
                                    ast::ExprIdx lhs,
                                    ast::ExprIdx rhs,
                                    diag::Span span,
                                    std::string_view what,
                                    const ir::TypeIdx* expected);
  void check_call_args(u32 module,
                       std::span<const ast::ExprIdx> args,
                       const std::vector<ir::TypeIdx>& params,
                       const std::vector<bool>& comp_params,
                       diag::Span span,
                       std::string_view what,
                       bool skip_first);
  bool is_core_fmt(const CheckedModule::FnSig* fn) const;
  bool is_io_print(const CheckedModule::FnSig* fn) const;
  std::vector<bool> comp_param_flags(ast::ItemIdx item) const;
  bool comp_checked_in_scope(ast::ExprIdx init) const;
  bool is_const_item(u32 module, ast::PathIdx path) const;
  // Bounds the recursive tree walk; see base::MAX_NESTING. A long
  // operator chain parses in a loop, so the parser's own descent does
  // not bound the tree it produces and this pass has to.
  base::NestingGuard nesting_{base::MAX_NESTING};
  bool reported_too_deep_ = false;
  void report_too_deep(diag::Span span);
  ir::TypeIdx check_path_expr(u32 module,
                              ast::ExprIdx expr,
                              std::span<const ast::TypeIdx> type_args,
                              ast::PathIdx path,
                              const ir::TypeIdx* expected,
                              diag::Span span);
  ir::TypeIdx check_fmt_write(u32 module,
                              ast::ExprIdx expr,
                              const ir::TypeIdx* expected,
                              const CheckedModule::FnSig* fn);
  bool verify_fmt_literal(ast::ExprIdx fmt_expr,
                          diag::Span args_span,
                          const std::vector<ir::TypeIdx>& elements,
                          diag::Span span);
  ir::TypeIdx check_fmt_format(u32 module,
                               ast::ExprIdx expr,
                               const ir::TypeIdx* expected,
                               const CheckedModule::FnSig* fn);
  ir::TypeIdx check_io_print(u32 module,
                             ast::ExprIdx expr,
                             const ir::TypeIdx* expected,
                             const CheckedModule::FnSig* fn);
  ir::TypeIdx check_call(u32 module,
                         ast::ExprIdx expr,
                         const ir::TypeIdx* expected);
  // A callee reached through a resolved signature: an unsafe one needs
  // an `unsafe { ... }` block at the call site (ADR-0050).
  void check_unsafe_call(const CheckedModule::FnSig* fn, diag::Span span);
  // A `comp fn` call happens during compilation only; outside comp
  // evaluation it is an error (ADR-0054).
  void check_comp_fn_call(const CheckedModule::FnSig* fn, diag::Span span);
  // An operation the gate covers must name it: with no enclosing
  // `unsafe { ... }` block, the diagnostic names the operation
  // (ADR-0050).
  void require_unsafe(diag::Span span, std::string_view what);
  // A closure literal: parameters bind from annotations or the
  // expected function type, the body checks as a function body,
  // and the value's type is the signature they make.
  ir::TypeIdx check_closure(u32 module,
                            ast::ExprIdx expr,
                            const ir::TypeIdx* expected);
  // A call through a function value: arity and argument types
  // unify against the signature, and the call records for
  // lowering, which emits it indirectly.
  ir::TypeIdx check_indirect_call(u32 module,
                                  ast::ExprIdx expr,
                                  ast::ExprIdx callee,
                                  const ir::FuncType& sig,
                                  const ir::TypeIdx* expected);
  ir::TypeIdx check_method_call(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected);
  ir::TypeIdx check_field(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected);
  ir::TypeIdx check_struct_expr(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected);
  ir::TypeIdx check_range(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected);
  ir::TypeIdx check_question(u32 module,
                             ast::ExprIdx expr,
                             const ir::TypeIdx* expected);
  ir::TypeIdx check_cast(u32 module,
                         ast::ExprIdx expr,
                         const ir::TypeIdx* expected);
  ir::TypeIdx check_index(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected);
  // `a[i]` where the receiver is a nominal type: resolves the `Index`
  // or `IndexMut` spec implementation, checks the index argument
  // against the spec's parameter, and records the call lowering emits.
  // `mutating` selects the spec, which is what a place position needs.
  ir::TypeIdx check_spec_index(u32 module,
                               ast::ExprIdx expr,
                               ir::TypeIdx receiver,
                               ir::TypeIdx position,
                               ast::ExprIdx index_expr,
                               const ir::TypeIdx* expected,
                               bool mutating);
  // `a == b` over a nominal type: resolves the `PartialEq`
  // implementation and records the call lowering emits.
  ir::TypeIdx check_spec_equality(u32 module,
                                  ast::ExprIdx expr,
                                  ir::TypeIdx type,
                                  const ir::TypeIdx* expected);
  // `a op b` (and `-a`) over a nominal type: resolves the operator
  // spec's implementation and records the call lowering emits. The
  // spec is named by the operator's spelling (`Add`, `neg`), which is
  // what its canonical shape fixed.
  ir::TypeIdx check_spec_arithmetic(u32 module,
                                    ast::ExprIdx expr,
                                    ir::TypeIdx type,
                                    std::string_view spec_name,
                                    std::string_view method_name,
                                    u32 params,
                                    const ir::TypeIdx* expected);
  // `a < b` and its siblings over a nominal type: resolves the
  // `PartialOrd` implementation and records the call lowering emits.
  ir::TypeIdx check_spec_ordering(u32 module,
                                  ast::ExprIdx expr,
                                  ir::TypeIdx type,
                                  const ir::TypeIdx* expected);
  // Element access: `receiver[integer]`, already checked. The expected
  // type, when given, is the element type.
  ir::TypeIdx check_element_index(ir::TypeIdx receiver,
                                  ir::TypeIdx position,
                                  ast::ExprIdx index_expr,
                                  const ir::TypeIdx* expected,
                                  diag::Span span);
  // Run access: `receiver[range]`, the range already checked. A fixed
  // array yields the bare `[E]`, which `unsized` reports: only a
  // borrow can name it. A slice reference re-slices to its own kind,
  // and `str` to `str`.
  ir::TypeIdx check_run_index(ir::TypeIdx receiver,
                              ir::TypeIdx element,
                              const ir::TypeIdx* expected,
                              diag::Span span,
                              bool& unsized);
  // `&a[range]` and its `mut` form. The borrow names the run of a
  // fixed array; borrowing a view has no place behind it, so it is
  // rejected here rather than in lowering.
  ir::TypeIdx check_borrow_of_index(u32 module,
                                    ast::ExprIdx expr,
                                    const ast::ExprBorrow& borrow,
                                    const ir::TypeIdx* expected);
  // The element type of a `Range<T>` instantiation; false for any
  // other type.
  bool range_element(ir::TypeIdx type, ir::TypeIdx& element) const;
  void check_cond(u32 module, ast::CondIdx cond, bool& binds);
  ir::TypeIdx check_if(u32 module,
                       ast::ExprIdx expr,
                       const ir::TypeIdx* expected);
  void check_exhaustive(u32 module,
                        ir::TypeIdx scrutinee,
                        std::span<const ast::ExprMatchArm> arms,
                        diag::Span span);
  bool pattern_is_wildcard(ast::PatternIdx pattern) const;
  void collect_bool_literals(ast::PatternIdx pattern,
                             std::vector<bool>& covered) const;
  void mark_variant_covered(ast::PatternIdx pattern,
                            std::span<const ast::ItemEnumVariant> variants,
                            std::vector<bool>& covered);
  ir::TypeIdx check_match(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected);
  ir::TypeIdx check_expr(u32 module,
                         ast::ExprIdx expr,
                         const ir::TypeIdx* expected);
  ir::TypeIdx check_array(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected);
  ir::TypeIdx check_expr_inner(u32 module,
                               ast::ExprIdx expr,
                               const ir::TypeIdx* expected);
  ir::TypeIdx check_block(u32 module,
                          ast::BlockIdx block,
                          const ir::TypeIdx* expected);
  ir::TypeIdx check_place(u32 module, ast::ExprIdx place);
  // Checks one statement and answers the type an expression
  // statement left behind, void otherwise; a block reads `!` from it
  // to know the statement does not fall through.
  ir::TypeIdx check_stmt(u32 module, ast::StmtIdx stmt);
  bool contains_mut_ref(ir::TypeIdx idx, std::vector<u32>& visited);
  // Whether a value of the type copies, mirroring `ir::is_copy_type`.
  // The checker's own walk so an in-progress recursive type cannot
  // recurse forever; a revisit answers Copy, because the cycle itself
  // is the error the checker reports elsewhere.
  bool capture_is_copy(ir::TypeIdx idx, std::vector<u32>& visited);
  void check_fn(u32 module, ast::ItemIdx fn, const ir::TypeIdx* self);
  void check_main(u32 module, ast::ItemIdx fn);
  void check_bodies();
  CheckedModule empty_module(u32 module);
};

}  // namespace analyzer
