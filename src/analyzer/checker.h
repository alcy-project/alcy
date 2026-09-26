// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

// Diagnostic codes 4010-4019 are reserved for type checking.
constexpr u32 ANALYZER_RECURSIVE_TYPE = 4010;
constexpr u32 ANALYZER_UNKNOWN_TYPE = 4011;
constexpr u32 ANALYZER_DUPLICATE_DEFINITION = 4012;
constexpr u32 ANALYZER_RESERVED_NAME = 4013;
constexpr u32 ANALYZER_ARITY_MISMATCH = 4014;
constexpr u32 ANALYZER_GENERIC_ARGUMENTS = 4015;
constexpr u32 ANALYZER_UNSUPPORTED_TYPE = 4016;
// Checked types failed storage verification on the way out.
constexpr u32 ANALYZER_INVALID_IR = 4017;
// Diagnostic codes 4020-4039 are reserved for expression checking.
constexpr u32 ANALYZER_TYPE_MISMATCH = 4020;
constexpr u32 ANALYZER_UNKNOWN_VALUE = 4021;
constexpr u32 ANALYZER_ARITY_ERROR = 4022;
constexpr u32 ANALYZER_INVALID_OPERATION = 4023;
constexpr u32 ANALYZER_NON_EXHAUSTIVE_MATCH = 4024;
constexpr u32 ANALYZER_REFUTABLE_LET = 4025;
constexpr u32 ANALYZER_MUST_USE = 4026;
constexpr u32 ANALYZER_BAD_QUESTION = 4027;
constexpr u32 ANALYZER_BAD_RETURN = 4028;
constexpr u32 ANALYZER_BAD_ASSIGNMENT = 4029;
constexpr u32 ANALYZER_BREAK_OUTSIDE_LOOP = 4030;
constexpr u32 ANALYZER_UNSUPPORTED_EXPR = 4031;
constexpr u32 ANALYZER_NOT_COMP_KNOWN = 4032;
constexpr u32 ANALYZER_INVALID_COMP = 4033;
constexpr u32 ANALYZER_UNKNOWN_INTRINSIC = 4034;
// Diagnostic codes 4040-4049 are reserved for destructors.
constexpr u32 ANALYZER_BAD_DROP_SIGNATURE = 4040;
constexpr u32 ANALYZER_DROP_ON_COPY = 4041;

// Name-interning map capacity (power of two, fixed: the table never
// resizes and traps on overflow, so size for programs, not tests).
constexpr u32 INTERNER_CAPACITY = 1u << 16;
constexpr u32 NO_MODULE = 0xFFFFFFFFu;

struct NominalEntry {
  u32 module;
  std::string_view name;
  ast::ItemIdx item;
  diag::Span span;
  ir::TypeIdx type;
  bool started = false;
  bool complete = false;
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
          diag::DiagBag& bag);

  const ModuleTree& tree;
  ast::AstArena& ast;
  ir::PointerWidth width;
  diag::DiagBag& bag;
  ir::StorageBuilder builder;
  str::StringInterner interner;
  std::vector<NominalEntry> nominals;
  std::vector<GenericInstance> generic_instances;
  std::vector<FnInstance> fn_instances;
  // Shared instantiation numbering that keys lowering side tables.
  // Holds each generic enum/struct instance type in creation order and
  // an invalid placeholder per generic function body, so both kinds
  // draw keys from one space.
  std::vector<ir::TypeIdx> inst_numbering;
  // `ref_type` appends a storage copy of a type so a struct's field
  // range stays contiguous. This maps each copy back to the type it
  // was copied from, so owner lookups accept both indexes.
  std::vector<std::pair<ir::TypeIdx, ir::TypeIdx>> type_origins_;
  // `MaybeUninit<T>` wrappers interned so far, as (wrapper, payload).
  std::vector<std::pair<ir::TypeIdx, ir::TypeIdx>> uninit_types_;
  // Destructor resolution, aligned with the type table by index.
  std::vector<CheckedModule::DropGlue> drop_glue_;
  std::vector<bool> needs_drop_;
  // Interned name every `MaybeUninit` wrapper carries.
  str::StringPoolId uninit_name_id = str::kInvalidStringPoolId;
  // Active type-parameter scope: innermost last. Pushed while
  // instantiating a generic enum or checking its members.
  std::vector<std::pair<std::string_view, ir::TypeIdx>> type_params;
  // Index into generic_instances while checking an instantiated
  // method body (NO_INST otherwise); keys the lowering side tables.
  u32 cur_inst = NO_INST;
  std::vector<CheckedModule> modules;
  std::vector<u32> parents;

  // Body-checking state, reset per function.
  struct Local {
    std::string_view name;
    ir::TypeIdx type;
    bool is_mut;
    bool comp_known = false;
  };
  bool in_fn = false;

  std::vector<std::vector<Local>> scopes;
  u32 loop_depth = 0;
  // Nonzero while checking comp evaluation contexts (comp block
  // contents and comp declaration initializers).
  u32 comp_depth = 0;
  // Verifies comp-known-ness while block scopes are still alive
  // (comp blocks and their nested bodies).
  bool verify_comp_known = false;
  ir::TypeIdx fn_ret = ir::TypeIdx(0);
  void register_nominals();
  NominalEntry* find_nominal(u32 module, std::string_view name);
  u32 find_child_module(u32 module, std::string_view name) const;
  ir::TypeIdx primitive_type(ast::PrimitiveKind kind, diag::Span span);
  ir::TypeIdx error_type();
  CheckedModule::ReceiverKind classify_receiver(ir::TypeIdx first,
                                                ir::TypeIdx self);
  ir::TypeIdx intern_nominal(NominalEntry& entry);
  // `MaybeUninit<T>` wrapper for `payload`, interned per payload.
  ir::TypeIdx intern_uninit(ir::TypeIdx payload);
  // Payload of a `MaybeUninit<T>` wrapper; invalid for any other type.
  ir::TypeIdx uninit_payload(ir::TypeIdx type) const;
  // Key of a type in the shared instantiation numbering, matching what
  // lowering side tables use; NO_INST for a non-instantiation.
  u32 inst_index(ir::TypeIdx type) const;
  // Declared type parameters of a function or method item.
  std::span<const ast::Ident> fn_generic_params(ast::ItemIdx item) const;
  // Item name for diagnostics.
  std::string_view fn_name(ast::ItemIdx item) const;
  // A type parameter a declared parameter type pins on its own. The
  // flags say where in the argument the bound type sits: `through_ref`
  // takes the pointee, `through_uninit` then unwraps the `MaybeUninit`
  // wrapper, which is how a generic intrinsic recovers `T` from a
  // `&mut MaybeUninit<T>` parameter.
  struct DeclaredBinding {
    u32 slot = 0;
    bool through_ref = false;
    bool through_uninit = false;
  };
  DeclaredBinding declared_binding(std::span<const ast::Ident> params,
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
  usize push_generic_scope(const GenericInstance& instance);
  void pop_generic_scope(usize kept);
  // Parameter names of a nominal declaration, whether struct or enum.
  std::span<const ast::Ident> nominal_params(const NominalEntry& entry);
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
                           const ir::TypeIdx* self);
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
  ir::TypeIdx unify(ir::TypeIdx expected,
                    ir::TypeIdx actual,
                    diag::Span span,
                    std::string_view what);
  const Local* lookup_local(std::string_view name) const;
  bool classify_suffix(std::string_view spelling,
                       ir::TypeTag& tag,
                       bool& is_float,
                       diag::Span span);
  ir::TypeIdx check_literal(ast::LiteralIdx value, const ir::TypeIdx* expected);
  void validate_cycles(const ir::Storage& storage);
  const CheckedModule::StaticInfo* lookup_static(u32 module,
                                                 std::string_view name) const;
  const CheckedModule::FnSig* lookup_function(u32 module,
                                              std::string_view name) const;
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
  // Finds an inherent method, instantiating generic impls on demand
  // (which checks the method body under the substitution).
  const CheckedModule::MethodInfo* lookup_method(ir::TypeIdx self,
                                                 std::string_view name,
                                                 u32 module,
                                                 diag::Span span);
  const CheckedModule::MethodInfo* instantiate_method(
      u32 impl_module,
      ir::TypeIdx self_type,
      const std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
      const ast::ItemImpl& impl,
      std::string_view name);
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

  struct PathValue {
    enum class Kind : u8 {
      Local,
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
  bool resolve_value_path(u32 module,
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
                                    std::string_view what);
  void check_call_args(u32 module,
                       std::span<const ast::ExprIdx> args,
                       const std::vector<ir::TypeIdx>& params,
                       const std::vector<bool>& comp_params,
                       diag::Span span,
                       std::string_view what,
                       bool skip_first);
  bool is_core_fmt(const CheckedModule::FnSig* fn) const;
  std::vector<bool> comp_param_flags(ast::ItemIdx item) const;
  bool comp_checked_in_scope(ast::ExprIdx init) const;
  bool is_literal_const(u32 module, ast::PathIdx path) const;
  ir::TypeIdx check_path_expr(u32 module,
                              std::span<const ast::TypeIdx> type_args,
                              ast::PathIdx path,
                              const ir::TypeIdx* expected,
                              diag::Span span);
  ir::TypeIdx check_fmt_write(u32 module,
                              ast::ExprIdx expr,
                              const ir::TypeIdx* expected,
                              const CheckedModule::FnSig* fn);
  bool verify_fmt_literal(ast::ExprIdx fmt_expr,
                          ast::ExprIdx args_expr,
                          const std::vector<ir::TypeIdx>& elements,
                          diag::Span span);
  ir::TypeIdx check_fmt_format(u32 module,
                               ast::ExprIdx expr,
                               const ir::TypeIdx* expected,
                               const CheckedModule::FnSig* fn);
  ir::TypeIdx check_call(u32 module,
                         ast::ExprIdx expr,
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
  ir::TypeIdx check_question(u32 module,
                             ast::ExprIdx expr,
                             const ir::TypeIdx* expected);
  ir::TypeIdx check_cast(u32 module,
                         ast::ExprIdx expr,
                         const ir::TypeIdx* expected);
  ir::TypeIdx check_index(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected);
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
  void check_stmt(u32 module, ast::StmtIdx stmt);
  bool contains_mut_ref(ir::TypeIdx idx, std::vector<u32>& visited);
  void check_fn(u32 module, ast::ItemIdx fn, const ir::TypeIdx* self);
  void check_main(u32 module, ast::ItemIdx fn);
  void check_bodies();
  CheckedModule empty_module(u32 module);
};

}  // namespace analyzer
