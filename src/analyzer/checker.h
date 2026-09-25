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

// Diagnostic codes 4210-4219 are reserved for type checking.
constexpr u32 kAnalyzerRecursiveType = 4210;
constexpr u32 kAnalyzerUnknownType = 4211;
constexpr u32 kAnalyzerDuplicateDefinition = 4212;
constexpr u32 kAnalyzerReservedName = 4213;
constexpr u32 kAnalyzerArityMismatch = 4214;
constexpr u32 kAnalyzerGenericArguments = 4215;
constexpr u32 kAnalyzerUnsupportedType = 4216;
// Diagnostic codes 4220-4239 are reserved for expression checking.
constexpr u32 kAnalyzerTypeMismatch = 4220;
constexpr u32 kAnalyzerUnknownValue = 4221;
constexpr u32 kAnalyzerArityError = 4222;
constexpr u32 kAnalyzerInvalidOperation = 4223;
constexpr u32 kAnalyzerNonExhaustiveMatch = 4224;
constexpr u32 kAnalyzerRefutableLet = 4225;
constexpr u32 kAnalyzerMustUse = 4226;
constexpr u32 kAnalyzerBadQuestion = 4227;
constexpr u32 kAnalyzerBadReturn = 4228;
constexpr u32 kAnalyzerBadAssignment = 4229;
constexpr u32 kAnalyzerBreakOutsideLoop = 4230;
constexpr u32 kAnalyzerUnsupportedExpr = 4231;
constexpr u32 kAnalyzerNotCompKnown = 4232;
constexpr u32 kAnalyzerInvalidComp = 4233;
constexpr u32 kAnalyzerUnknownIntrinsic = 4234;

// Name-interning map capacity (power of two, fixed: the table never
// resizes and traps on overflow, so size for programs, not tests).
constexpr u32 kInternerCapacity = 1u << 16;
constexpr u32 kNoModule = 0xFFFFFFFFu;

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
  // `ref_type` appends a storage copy of a type so a struct's field
  // range stays contiguous. This maps each copy back to the type it
  // was copied from, so owner lookups accept both indexes.
  std::vector<std::pair<ir::TypeIdx, ir::TypeIdx>> type_origins_;
  // Active type-parameter scope: innermost last. Pushed while
  // instantiating a generic enum or checking its members.
  std::vector<std::pair<std::string_view, ir::TypeIdx>> type_params;
  // Index into generic_instances while checking an instantiated
  // method body (kNoInst otherwise); keys the lowering side tables.
  u32 cur_inst = kNoInst;
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

  struct PathValue {
    enum class Kind : u8 {
      Local,
      Static,
      Function,
      AssocFunction,
      UnitVariant,
      TupleVariant,
      Type,
    };
    Kind kind = Kind::Type;
    ir::TypeIdx type = ir::TypeIdx(0);
    const CheckedModule::FnSig* function = nullptr;
    const CheckedModule::MethodInfo* method = nullptr;
    NominalEntry* enom = nullptr;
    u32 variant = 0;
  };
  bool resolve_value_path(u32 module, ast::PathIdx path, PathValue& out);
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
