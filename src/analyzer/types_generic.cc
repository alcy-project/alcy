// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/checker.h"
#include "analyzer/diag_code.h"
#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/type_util.h"

namespace analyzer {

void Checker::validate_generic_annotations() {
  // A parameter's annotation is a value type (`N: usize`) or a bound
  // (`T: Display + PartialEq`). The first is a primitive integer; every
  // other annotation has to name a spec (ADR-0028).
  const auto check = [&](u32 module,
                         std::span<const ast::GenericParam> params) {
    for (const ast::GenericParam& param : params) {
      if (is_value_param(ast, param)) {
        continue;
      }
      std::vector<ast::TypeIdx> annotations;
      if (param.type.is_valid()) {
        annotations.push_back(param.type);
      }
      for (ast::TypeIdx extra : param.bounds) {
        annotations.push_back(extra);
      }
      for (ast::TypeIdx annotation : annotations) {
        if (!annotation.is_valid()) {
          continue;
        }
        if (ast.types[annotation].kind == ast::TypeKind::Primitive) {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerValueParameterTypeNotInteger>(
                  DiagCode::TypeMismatch, param.name.span, param.name.name);
          (void)index;
          continue;
        }
        if (ast.types[annotation].kind != ast::TypeKind::Path) {
          const u32 index = bag.emit<i18n::Key::AnalyzerBoundNotASpec>(
              DiagCode::SpecSuperBadTarget, ast.types[annotation].span,
              param.name.name);
          (void)index;
          continue;
        }
        const ast::TypePath& path =
            ast.types[annotation].payload.get<ast::TypePath>();
        const std::span<const ast::Ident> segments =
            ast.paths[path.path].segments;
        const std::string_view name =
            segments.empty() ? std::string_view{} : segments.back().name;
        if (segments.empty() || bound_spec(module, name) == U32_MAX) {
          const u32 index = bag.emit<i18n::Key::AnalyzerBoundUnknownSpec>(
              DiagCode::SpecSuperBadTarget, ast.types[annotation].span, name);
          (void)index;
        }
      }
    }
  };
  for (const NominalEntry& entry : nominals) {
    check(entry.module, nominal_params(entry));
  }
  // Every declaration that may spell a value parameter owes the same
  // rule; a function's or impl's annotation is no less a type.
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    for (ast::ItemIdx item : tree.modules[m]->items) {
      const ast::ItemNode& node = ast.items[item];
      switch (node.kind) {
        case ast::ItemKind::Fn:
          check(m, node.payload.get<ast::ItemFn>().generic);
          break;
        case ast::ItemKind::Intrinsic:
          check(m, node.payload.get<ast::ItemIntrinsic>().generic);
          break;
        case ast::ItemKind::Extern:
          for (const ast::ItemExternFn& fn :
               node.payload.get<ast::ItemExtern>().fns) {
            check(m, fn.generic);
          }
          break;
        case ast::ItemKind::Impl:
          check(m, node.payload.get<ast::ItemImpl>().params);
          break;
        case ast::ItemKind::Spec: {
          const ast::ItemSpec& spec = node.payload.get<ast::ItemSpec>();
          check(m, spec.params);
          // A spec method's own type parameters carry bounds the same
          // way a function's do.
          for (const ast::SpecMethod& method : spec.methods) {
            check(m, method.generic);
          }
          break;
        }
        default: break;
      }
    }
  }
}

bool Checker::is_expanded_variadic(ast::ItemIdx item) const {
  if (!item.is_valid() || ast.items[item].kind != ast::ItemKind::Fn) {
    return false;
  }
  const std::string_view name =
      ast.items[item].payload.get<ast::ItemFn>().name.name;
  if (name == "write" || name == "format") {
    return tree.is_staged_item("fmt", item);
  }
  if (name == "print" || name == "println" || name == "eprint" ||
      name == "eprintln") {
    return tree.is_staged_item("io", item);
  }
  return false;
}

// Only a compiler-expanded function may declare `args: ..` (ADR-0055):
// the expansion gives the slot its meaning, and a declaration without
// one would be a function nobody could call.

void Checker::validate_variadic_parameters() {
  const auto check = [&](ast::ItemIdx item,
                         std::span<const ast::ItemFnParam> params) {
    for (const ast::ItemFnParam& param : params) {
      if (!param.is_variadic || is_expanded_variadic(item)) {
        continue;
      }
      const u32 index = bag.emit<i18n::Key::AnalyzerVariadicNotExpanded>(
          DiagCode::InvalidOperation, ast.patterns[param.pattern].span);
      (void)index;
    }
  };
  for (ModuleNode* module : tree.modules) {
    for (ast::ItemIdx item : module->items) {
      const ast::ItemNode& node = ast.items[item];
      switch (node.kind) {
        case ast::ItemKind::Fn:
          check(item, node.payload.get<ast::ItemFn>().params);
          break;
        case ast::ItemKind::Intrinsic:
          check(item, node.payload.get<ast::ItemIntrinsic>().params);
          break;
        case ast::ItemKind::Extern:
          for (const ast::ItemExternFn& fn :
               node.payload.get<ast::ItemExtern>().fns) {
            check(ast::ItemIdx::invalid(), fn.params);
          }
          break;
        default: break;
      }
    }
  }
}

const GenericInstance* Checker::generic_find(ir::TypeIdx idx) const {
  const auto found = instance_by_type_.find(idx.idx);
  return found == instance_by_type_.end() ? nullptr
                                          : &generic_instances[found->second];
}

u32 Checker::nominal_index(const NominalEntry* entry) {
  return static_cast<u32>(entry - nominals.data());
}

const GenericInstance* Checker::generic_instance_for(u32 nominal,
                                                     ir::TypeIdx type) const {
  const GenericInstance* instance = generic_find(type);
  if (instance == nullptr || instance->nominal != nominal) {
    return nullptr;
  }
  return instance;
}

u32 Checker::inst_index(ir::TypeIdx type) const {
  const auto found = inst_by_type_.find(type.idx);
  return found == inst_by_type_.end() ? NO_INST : found->second;
}

// Storage copies chain: a copy's origin can itself be a copy, so the
// chain is followed to the type the first copy was made from.

ir::TypeIdx Checker::type_origin(ir::TypeIdx type) const {
  ir::TypeIdx current = type;
  for (u32 depth = 0; depth <= type_origins_.size(); ++depth) {
    const auto found = type_origins_by_index_.find(current.idx);
    if (found == type_origins_by_index_.end()) {
      return current;
    }
    current = ir::TypeIdx(found->second);
  }
  return current;
}

std::span<const ast::GenericParam> Checker::nominal_params(
    const NominalEntry& entry) {
  const ast::ItemNode& node = ast.items[entry.item];
  if (node.kind == ast::ItemKind::Struct) {
    return node.payload.get<ast::ItemStruct>().params;
  }
  return node.payload.get<ast::ItemEnum>().params;
}

void Checker::pop_generic_scope(const GenericScope& kept) {
  while (type_params.size() > kept.type_params) {
    type_params.pop_back();
  }
  while (value_params.size() > kept.value_params) {
    value_params.pop_back();
  }
}

u64 Checker::const_type_value(ir::TypeIdx type) const {
  return builder.const_types()[builder.types()[type].as_const()].value;
}

ir::TypeIdx Checker::nominal_owner_type(NominalEntry* entry) {
  if (!nominal_params(*entry).empty()) {
    return error_type();
  }
  return intern_nominal(*entry);
}

// The caller re-checks each argument against the resolved payloads, so
// this pass only reads types and never commits to a payload check.

const GenericInstance* Checker::infer_from_payload_args(
    u32 module,
    u32 nominal,
    const PathValue& resolved,
    std::span<const ast::ExprIdx> args) {
  const ast::ItemEnum& decl =
      ast.items[nominals[nominal].item].payload.get<ast::ItemEnum>();
  const std::span<const ast::TypeIdx> fields =
      decl.variants[resolved.variant].fields;
  if (args.size() != fields.size()) {
    return nullptr;
  }
  std::vector<ir::TypeIdx> bound(decl.params.size(),
                                 ir::TypeIdx(base::INVALID_IDX));
  for (usize i = 0; i < fields.size(); ++i) {
    const ast::TypeNode& field = ast.types[fields[i]];
    if (field.kind != ast::TypeKind::Path ||
        !field.payload.get<ast::TypePath>().args.empty()) {
      continue;
    }
    const ast::Path& path = ast.paths[field.payload.get<ast::TypePath>().path];
    if (path.segments.size() != 1) {
      continue;
    }
    u32 slot = decl.params.size();
    for (u32 p = 0; p < decl.params.size(); ++p) {
      if (decl.params[p].name.name == path.segments[0].name) {
        slot = p;
        break;
      }
    }
    if (slot == decl.params.size()) {
      continue;
    }
    if (is_value_param(ast, decl.params[slot])) {
      // A value slot binds from a value argument, not from a payload's
      // type; the annotation path is the one that resolves it.
      return nullptr;
    }
    const ir::TypeIdx actual = check_expr(module, args[i], nullptr);
    if (is_error(actual)) {
      return nullptr;
    }
    if (bound[slot].is_valid() && bound[slot].idx != actual.idx) {
      // Conflicting bindings need full unification variables; leave
      // the case to the annotation path.
      return nullptr;
    }
    bound[slot] = actual;
  }
  for (const ir::TypeIdx arg : bound) {
    if (!arg.is_valid()) {
      return nullptr;
    }
  }
  return generic_find(instantiate_generic(nominal, bound, diag::Span{}));
}

// Interns one instantiation of a generic enum, substituting the
// entry's parameters with `args`. The index reserves first so
// recursive mentions of the same instantiation resolve to it.

ir::TypeIdx Checker::instantiate_generic(u32 nominal,
                                         const std::vector<ir::TypeIdx>& args,
                                         diag::Span span) {
  // The declaration's parameters are indexed below; a bad turbofish
  // reaches here from a value path, which has no arity check of its own.
  const std::span<const ast::GenericParam> params =
      nominal_params(nominals[nominal]);
  if (args.size() != params.size()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTypeArityMismatch>(
        DiagCode::ArityMismatch, span, nominals[nominal].name, params.size(),
        params.size() == 1 ? "" : "s");
    (void)index;
    return error_type();
  }
  // A nominal that declares no parameters is not an instantiation: the
  // declaration's own type is the answer, so a concrete type reached
  // through a substitution keeps the one index its fields, its
  // destructor, and its origin were interned under.
  if (params.empty()) {
    return intern_nominal(nominals[nominal]);
  }
  // ADR-0085: a type argument is Movable. This is where both a
  // spelled turbofish and an instantiated type annotation arrive.
  for (usize i = 0; i < args.size(); ++i) {
    if (is_value_param(ast, params[i])) {
      continue;
    }
    if (!is_error(args[i]) && !is_movable(args[i])) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnmovableTypeArgument>(
          DiagCode::UnmovableType, span);
      (void)index;
      return error_type();
    }
  }
  // A caller that reaches this from a value path resolves arguments
  // itself, so a value-declared slot must hold the const type a value
  // was lowered to. Anything else is a type argument in that slot.
  for (usize i = 0; i < params.size(); ++i) {
    if (!is_value_param(ast, params[i])) {
      continue;
    }
    if (is_error(args[i]) || tag_of(args[i]) != ir::TypeTag::Const) {
      const u32 index = bag.emit<i18n::Key::AnalyzerValueArgumentExpected>(
          DiagCode::TypeMismatch, span, params[i].name.name);
      (void)index;
      return error_type();
    }
  }
  // An instance already minted for these arguments is the one to reuse. The
  // search is over the instances of this nominal: an instantiation request
  // scans them, and a package requests one per site that spells it.
  if (nominal < instances_by_nominal_.size()) {
    for (u32 index : instances_by_nominal_[nominal]) {
      const GenericInstance& instance = generic_instances[index];
      if (instance.args == args) {
        return instance.type;
      }
    }
    // Index equality under-compares compound arguments: tuple types are
    // re-minted on each resolution, so two spellings of one
    // instantiation carry different indices. Fall back to structural
    // equality before minting another; instances are interned once and
    // shared by identity.
    for (u32 index : instances_by_nominal_[nominal]) {
      const GenericInstance& instance = generic_instances[index];
      if (instance.args.size() != args.size()) {
        continue;
      }
      bool same = true;
      for (usize i = 0; same && i < args.size(); ++i) {
        same = types_equal(instance.args[i], args[i]);
      }
      if (same) {
        return instance.type;
      }
    }
  }
  const NominalEntry& entry = nominals[nominal];
  const ast::ItemNode& node = ast.items[entry.item];
  const str::StringPoolId name = intern_name(entry.name);
  if (name == str::INVALID_STRING_POOL_ID) {
    return error_type();
  }
  const bool is_struct = node.kind == ast::ItemKind::Struct;
  // Every variant name is interned before the instance is recorded, so a
  // spent table leaves no half-built instantiation behind.
  std::vector<str::StringPoolId> variant_names;
  if (!is_struct) {
    for (const ast::ItemEnumVariant& variant :
         node.payload.get<ast::ItemEnum>().variants) {
      const str::StringPoolId variant_name = intern_name(variant.name.name);
      if (variant_name == str::INVALID_STRING_POOL_ID) {
        return error_type();
      }
      variant_names.push_back(variant_name);
    }
  }
  const ir::TypeIdx reserved =
      is_struct ? builder.reserve_struct(name) : builder.reserve_enum(name);
  generic_instances.push_back(GenericInstance{nominal, args, reserved});
  const usize instance_index = generic_instances.size() - 1;
  instance_by_type_.emplace(reserved.idx, static_cast<u32>(instance_index));
  if (instances_by_nominal_.size() <= nominal) {
    instances_by_nominal_.resize(nominal + 1);
  }
  instances_by_nominal_[nominal].push_back(static_cast<u32>(instance_index));
  inst_numbering.push_back(reserved);
  // The numbering's own index, which is not the instance's index: a generic
  // function claims a slot of the numbering without minting a type.
  inst_by_type_.emplace(reserved.idx,
                        static_cast<u32>(inst_numbering.size() - 1));
  // A sequence must be contiguous in the type table, and the arguments
  // are arbitrary existing nodes, so each is copied in.
  ir::TypeSeq args_seq;
  for (ir::TypeIdx arg : args) {
    args_seq.push(builder.ref_type(arg));
  }
  const usize pushed = type_params.size();
  const usize pushed_values = value_params.size();
  const auto push_bindings = [&](std::span<const ast::GenericParam> declared) {
    for (usize i = 0; i < declared.size(); ++i) {
      if (is_value_param(ast, declared[i])) {
        value_params.push_back({declared[i].name.name,
                                const_type_value(args[i]), entry.module,
                                declared[i].type});
      } else {
        type_params.emplace_back(declared[i].name.name, args[i]);
      }
    }
  };
  if (is_struct) {
    push_bindings(node.payload.get<ast::ItemStruct>().params);
    std::vector<ir::TypeIdx> fields;
    fields.reserve(node.payload.get<ast::ItemStruct>().fields.size());
    for (const ast::ItemStructField& field :
         node.payload.get<ast::ItemStruct>().fields) {
      fields.push_back(resolve_type(entry.module, field.type, nullptr));
    }
    // The copies must be adjacent, so every field resolves before any
    // copy is appended: resolution can create types of its own.
    ir::TypeSeq seq;
    for (ir::TypeIdx field : fields) {
      seq.push(storage_copy(field));
    }
    builder.fill_struct(reserved, seq.finish(), args_seq.finish());
  } else {
    const ast::ItemEnum& decl = node.payload.get<ast::ItemEnum>();
    push_bindings(decl.params);
    // Payloads resolve first so variant nodes append back-to-back.
    std::vector<std::vector<ir::TypeIdx>> payloads;
    payloads.reserve(decl.variants.size());
    for (const ast::ItemEnumVariant& variant : decl.variants) {
      std::vector<ir::TypeIdx> fields;
      fields.reserve(variant.fields.size());
      for (ast::TypeIdx field : variant.fields) {
        fields.push_back(resolve_type(entry.module, field, nullptr));
      }
      payloads.push_back(std::move(fields));
    }
    ir::EnumVariantTypeSeq variants;
    for (usize i = 0; i < variant_names.size(); ++i) {
      ir::TypeSeq seq;
      for (ir::TypeIdx field : payloads[i]) {
        seq.push(storage_copy(field));
      }
      variants.push(builder.enum_variant(variant_names[i], seq.finish()));
    }
    builder.fill_enum(reserved, variants.finish(), args_seq.finish());
  }
  while (type_params.size() > pushed) {
    type_params.pop_back();
  }
  while (value_params.size() > pushed_values) {
    value_params.pop_back();
  }
  // A field type can instantiate another generic nominal, which pushes
  // entries of its own; this instance's entry is the one recorded above.
  GenericInstance& instance = generic_instances[instance_index];
  instance.started = true;
  instance.complete = true;
  return reserved;
}

// Resolves all path segments but the last to a module. Shared by type
// and value paths; `what` names the namespace for diagnostics.

ast::ItemIdx Checker::lookup_generic_fn(u32 module, std::string_view name) {
  for (ast::ItemIdx item : tree.modules[module]->items) {
    if (fn_name(item) == name && !fn_generic_params(item).empty()) {
      return item;
    }
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Value || import.name != name) {
      continue;
    }
    for (ast::ItemIdx item : tree.modules[import.target_module]->items) {
      if (fn_name(item) == import.member && !fn_generic_params(item).empty()) {
        return item;
      }
    }
  }
  return ast::ItemIdx::invalid();
}

void Checker::index_generic_impls() {
  generic_impls_by_nominal_.assign(nominals.size(), {});
  indexed_nominals_ = nominals.size();
  generic_impls_indexed_ = true;
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    for (ast::ItemIdx item : tree.modules[m]->items) {
      const ast::ItemNode& node = ast.items[item];
      if (node.kind != ast::ItemKind::Impl) {
        continue;
      }
      const ast::ItemImpl& impl = node.payload.get<ast::ItemImpl>();
      // A spec impl's methods dispatch through their spec; only an
      // inherent block belongs to the inherent lookup this feeds.
      if (impl.spec.is_valid() || impl.params.empty()) {
        continue;
      }
      const ast::TypeNode& target = ast.types[impl.type];
      if (target.kind != ast::TypeKind::Path) {
        continue;
      }
      std::string_view target_name;
      NominalEntry* target_entry = find_impl_target(m, impl.type, target_name);
      if (target_entry == nullptr) {
        continue;
      }
      generic_impls_by_nominal_[nominal_index(target_entry)].emplace_back(m,
                                                                          item);
    }
  }
}

void Checker::index_generic_methods() {
  generic_methods_by_nominal_.assign(nominals.size(), {});
  generic_methods_indexed_ = true;
  indexed_nominals_ = nominals.size();
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    for (ast::ItemIdx item : tree.modules[m]->items) {
      const ast::ItemNode& node = ast.items[item];
      if (node.kind != ast::ItemKind::Impl) {
        continue;
      }
      const ast::ItemImpl& impl = node.payload.get<ast::ItemImpl>();
      // A generic spec impl's methods instantiate through the declared
      // signature, which is the check; a concrete one's conformance was
      // settled where the impl was written. A method that declares type
      // parameters of its own is neither: its signature does not exist
      // until a call names them, so it indexes here in both shapes and
      // the call resolves it against the declaration.
      if (impl.spec.is_valid() && !impl.params.empty()) {
        bool any_generic = false;
        for (ast::ItemIdx method_item : impl.methods) {
          if (!ast.items[method_item]
                   .payload.get<ast::ItemFn>()
                   .generic.empty()) {
            any_generic = true;
            break;
          }
        }
        if (!any_generic) {
          continue;
        }
      }
      const ast::TypeNode& target = ast.types[impl.type];
      if (target.kind != ast::TypeKind::Path) {
        continue;
      }
      std::string_view target_name;
      NominalEntry* target_entry = find_impl_target(m, impl.type, target_name);
      if (target_entry == nullptr) {
        continue;
      }
      for (ast::ItemIdx method_item : impl.methods) {
        if (ast.items[method_item].payload.get<ast::ItemFn>().generic.empty()) {
          continue;
        }
        generic_methods_by_nominal_[nominal_index(target_entry)].push_back(
            GenericMethodEntry{m, item, method_item});
      }
    }
  }
}

const CheckedModule::MethodInfo* Checker::lookup_method(
    ir::TypeIdx self,
    std::string_view name,
    u32 module,
    diag::Span span,
    bool spec_only,
    u32 spec_filter,
    std::span<const ast::TypeIdx> method_type_args) {
  bound_failure_ = false;
  // A field's type is a storage copy of the declared type, so a method or
  // operator reached through a field follows the copy back to its origin.
  self = type_origin(self);
  // A method on a type parameter is the bound's (ADR-0080): the
  // parameter's declared bounds are what its body may call, so the
  // dispatch resolves through them rather than through a nominal.
  if (tag_of(self) == ir::TypeTag::Param) {
    return lookup_param_method(self, name, module, span, spec_only, spec_filter,
                               method_type_args);
  }
  // A call that spells type arguments reaches only a method that
  // declares them; the eager inherent methods declare none.
  if (!spec_only && method_type_args.empty()) {
    if (const CheckedModule::MethodInfo* inherent =
            lookup_inherent_method(self, name, module, span)) {
      return inherent;
    }
  }
  // Spec dispatch: `impl S for T` blocks, after inherent methods.
  // Coherence leaves at most one impl per spec overlapping a concrete
  // type, so every in-scope match is a different spec; two providing the
  // same name is an ambiguity, not a choice.
  u32 target_nominal = 0;
  std::vector<ir::TypeIdx> target_args;
  bool has_nominal = false;
  if (const GenericInstance* instance = generic_find(self)) {
    target_nominal = instance->nominal;
    target_args = instance->args;
    has_nominal = true;
  } else {
    const auto declaring = nominal_by_type_.find(self.idx);
    if (declaring != nominal_by_type_.end()) {
      target_nominal = declaring->second;
      has_nominal = true;
    }
  }
  const CheckedModule::MethodInfo* match = nullptr;
  for (const SpecImplEntry& entry : spec_impls) {
    bool generic_record = false;
    std::vector<std::pair<std::string_view, ir::TypeIdx>> scope;
    std::vector<ValueParam> value_scope;
    bool shape_ok = true;
    if (entry.target.is_primitive) {
      // A scalar target has no arguments and is never generic: the
      // receiver must be that scalar itself.
      if (!types_equal(entry.target.primitive, self)) {
        continue;
      }
    } else {
      if (!has_nominal || entry.target.nominal != target_nominal ||
          entry.target.args.size() != target_args.size()) {
        continue;
      }
      for (usize i = 0; i < target_args.size() && shape_ok; ++i) {
        if (entry.target.args[i].is_value) {
          generic_record = true;
          if (tag_of(target_args[i]) != ir::TypeTag::Const) {
            shape_ok = false;
            break;
          }
          value_scope.push_back({entry.target.args[i].param,
                                 const_type_value(target_args[i]), entry.module,
                                 entry.target.args[i].annotation});
        } else if (entry.target.args[i].is_param) {
          generic_record = true;
          scope.emplace_back(entry.target.args[i].param, target_args[i]);
        } else if (!types_equal(entry.target.args[i].type, target_args[i])) {
          shape_ok = false;
        }
      }
      if (!shape_ok) {
        continue;
      }
    }
    if (spec_filter != U32_MAX && entry.spec != spec_filter) {
      continue;
    }
    // An operator reaches the compiler's spec by identity, so it needs
    // no `use`; every other spec lookup follows scope.
    if (spec_filter == U32_MAX && !spec_in_scope(module, specs[entry.spec])) {
      continue;
    }
    const CheckedModule::MethodInfo* candidate = nullptr;
    if (generic_record) {
      const ast::ItemImpl& impl =
          ast.items[entry.item].payload.get<ast::ItemImpl>();
      bool provides = false;
      for (ast::ItemIdx method_item : impl.methods) {
        if (ast.items[method_item].payload.get<ast::ItemFn>().name.name ==
            name) {
          provides = true;
          break;
        }
      }
      if (!provides) {
        continue;
      }
    } else {
      const auto own = methods_by_self_.find(self.idx);
      if (own != methods_by_self_.end()) {
        for (const auto& [owner, position] : own->second) {
          const CheckedModule::MethodInfo& method =
              modules[owner].methods[position];
          // An entry keyed by spelled arguments is one call's
          // instantiation, not the impl's registration: the registry
          // re-instantiates per call, so the scan takes only the
          // registered method.
          if (method.spec == entry.spec && method.name == name &&
              method.method_args.empty()) {
            candidate = &method;
            break;
          }
        }
      }
      if (candidate == nullptr) {
        continue;
      }
    }
    if (match != nullptr) {
      const u32 index = bag.emit<i18n::Key::AnalyzerAmbiguousMethod>(
          DiagCode::UnknownValue, span, name);
      (void)index;
      return nullptr;
    }
    if (generic_record) {
      const ast::ItemImpl& impl =
          ast.items[entry.item].payload.get<ast::ItemImpl>();
      std::vector<ir::TypeIdx> record_spec_args;
      for (const SpecTarget::Arg& shape : entry.spec_args) {
        if (shape.shape.is_valid()) {
          const usize pushed = type_params.size();
          for (const auto& binding : scope) {
            type_params.emplace_back(binding.first, binding.second);
          }
          const ir::TypeIdx resolved =
              resolve_type(entry.module, shape.shape, nullptr);
          while (type_params.size() > pushed) {
            type_params.pop_back();
          }
          if (is_error(resolved)) {
            shape_ok = false;
            break;
          }
          record_spec_args.push_back(resolved);
          continue;
        }
        if (!shape.is_param) {
          record_spec_args.push_back(shape.type);
          continue;
        }
        ir::TypeIdx bound = error_type();
        for (const auto& binding : scope) {
          if (binding.first == shape.param) {
            bound = binding.second;
            break;
          }
        }
        if (is_error(bound)) {
          shape_ok = false;
          break;
        }
        record_spec_args.push_back(bound);
      }
      if (!shape_ok) {
        continue;
      }
      // The impl's bounds have to hold for this instantiation before
      // its body specializes (ADR-0028): the refusal names the type
      // the caller passed rather than a line inside the library.
      if (!check_declared_bounds(entry.module, impl.params, scope, span)) {
        continue;
      }
      // A method with its own type parameters is instantiated from the
      // call's spelled arguments, resolved where the receiver's are.
      std::vector<ir::TypeIdx> method_args;
      method_args.reserve(method_type_args.size());
      bool args_ok = true;
      for (ast::TypeIdx arg : method_type_args) {
        const ir::TypeIdx resolved = resolve_type(module, arg, &self);
        if (is_error(resolved)) {
          args_ok = false;
          break;
        }
        method_args.push_back(resolved);
      }
      if (!args_ok) {
        return nullptr;
      }
      // ADR-0085: a type argument is Movable, the spec method's own
      // included.
      if (!check_type_args_movable(method_args, span)) {
        return nullptr;
      }
      candidate = instantiate_spec_method(entry.module, entry.spec,
                                          record_spec_args, method_args, self,
                                          scope, value_scope, impl, name, span);
      if (candidate == nullptr) {
        return nullptr;
      }
    }
    match = candidate;
  }
  return match;
}

ir::TypeIdx Checker::own_fn_return_witness(
    ast::ItemIdx item,
    usize slot,
    std::span<const ir::TypeIdx> args) const {
  for (const OwnFnReturnWitness& recorded : own_fn_return_witnesses_) {
    if (recorded.item != item || recorded.slot != slot) {
      continue;
    }
    if (recorded.args.size() != args.size() ||
        !std::equal(recorded.args.begin(), recorded.args.end(), args.begin())) {
      continue;
    }
    return recorded.witness;
  }
  return ir::TypeIdx::invalid();
}

bool Checker::try_bind_own_fn_return(u32 module,
                                     ir::TypeIdx& fn_ret,
                                     ast::ExprIdx value,
                                     diag::Span span) {
  const PendingOwnFnReturn* found = nullptr;
  // The innermost pending answers first: a nested instantiation appends
  // while an outer body is mid-check, and each answers its own stand-in.
  for (const PendingOwnFnReturn& pending : pending_own_fn_returns_) {
    if (pending.stand_in.idx == fn_ret.idx) {
      found = &pending;
    }
  }
  if (found == nullptr) {
    return false;
  }
  // The record is copied before the value is checked: a nested
  // instantiation may append to the list while it runs.
  const PendingOwnFnReturn pending = *found;
  const OwnFnSlot& declared = *pending.declared;
  const ir::TypeIdx actual = check_expr(module, value, nullptr);
  bool compared = false;
  bool matches = false;
  if (!is_error(actual)) {
    std::vector<ir::TypeIdx> want_params;
    want_params.reserve(declared.sig.params.size());
    for (ast::TypeIdx param : declared.sig.params) {
      want_params.push_back(resolve_type(module, param, nullptr));
    }
    const ir::TypeIdx want_ret =
        resolve_type(module, declared.sig.ret, nullptr);
    if (!is_error(want_ret)) {
      compared = true;
      matches = is_callable_with(actual, want_params, want_ret);
    }
  }
  if (matches) {
    own_fn_return_witnesses_.push_back(
        OwnFnReturnWitness{pending.item, pending.slot, pending.args, actual});
    fn_ret = actual;
  } else {
    if (compared) {
      const u32 index = bag.emit<i18n::Key::AnalyzerOwnFunctionReturn>(
          DiagCode::TypeMismatch, span, declared.display);
      (void)index;
    }
    // An answer that failed: later returns meet the error, and the
    // instantiation reports nothing further.
    own_fn_return_witnesses_.push_back(OwnFnReturnWitness{
        pending.item, pending.slot, pending.args, error_type()});
    fn_ret = error_type();
  }
  return true;
}

void Checker::desugar_own_fn_params(ast::ItemIdx item) {
  const ast::ItemNode& node = ast.items[item];
  if (node.kind != ast::ItemKind::Fn) {
    return;
  }
  // The copy is what the payload is rewritten from: the union has no
  // mutable accessor, so a changed item is set back whole.
  ast::ItemFn fn = node.payload.get<ast::ItemFn>();
  std::vector<ast::GenericParam> added;
  // One spelling: the type becomes a path to a fresh synthetic
  // parameter, the item's generic list grows, and the signature is
  // recorded for the witness check. `display` names the spelling in a
  // diagnostic, empty when only the synthetic name can.
  auto rewrite = [&](ast::TypeIdx spelling, ast::TypeFunc sig, bool is_return,
                     std::string_view display) {
    own_fn_names_.push_back("__fn" + std::to_string(next_own_fn_++));
    const ast::Ident name{own_fn_names_.back(), ast.types[spelling].span};
    const diag::Span span = ast.types[spelling].span;
    // The parameter is bounded by the spec its spelling names
    // (ADR-0116): `fn(A, B) -> R` is `Fn<(A, B), R>`, with the
    // parameters as one tuple so one spec covers every arity.
    const ast::Ident spec_name{"Fn", span};
    const std::vector<ast::Ident> spec_segments{spec_name};
    const ast::PathIdx spec_path = ast.paths.push_back(
        ast::Path{ast::copy_to_arena(ast.spans, spec_segments), span});
    ast::TypeNode params_node;
    params_node.kind = ast::TypeKind::Tuple;
    params_node.span = span;
    params_node.payload.set(ast::TypeTuple{.elements = sig.params});
    const ast::TypeIdx params_type = ast.types.push_back(params_node);
    const std::vector<ast::TypeIdx> bound_args{params_type, sig.ret};
    ast::TypeNode bound_node;
    bound_node.kind = ast::TypeKind::Path;
    bound_node.span = span;
    bound_node.payload.set(ast::TypePath{
        .path = spec_path, .args = ast::copy_to_arena(ast.spans, bound_args)});
    const ast::TypeIdx bound_type = ast.types.push_back(bound_node);
    const std::vector<ast::Ident> segments{name};
    const ast::PathIdx path = ast.paths.push_back(
        ast::Path{ast::copy_to_arena(ast.spans, segments), span});
    ast.types[spelling].kind = ast::TypeKind::Path;
    ast.types[spelling].payload.set(ast::TypePath{.path = path, .args = {}});
    added.push_back(
        ast::GenericParam{.name = name, .type = bound_type, .bounds = {}});
    own_fn_slots_.push_back(OwnFnSlot{name.name, sig, is_return,
                                      display.empty() ? name.name : display});
  };
  for (const ast::ItemFnParam& param : fn.params) {
    const ast::TypeIdx param_type = param.type;
    if (!param_type.is_valid() ||
        ast.types[param_type].kind != ast::TypeKind::OwnFunc) {
      continue;
    }
    const ast::TypeFunc sig =
        ast.types[param_type].payload.get<ast::TypeFunc>();
    const ast::PatternKind kind = ast.patterns[param.pattern].kind;
    std::string_view display{};
    if (kind == ast::PatternKind::Ident) {
      display = ast.patterns[param.pattern].payload.ident.name.name;
    } else if (kind == ast::PatternKind::MutIdent) {
      display = ast.patterns[param.pattern].payload.mut_ident.name.name;
    }
    rewrite(param_type, sig, false, display);
  }
  // A return-position spelling waits for the body to name its witness
  // (ADR-0075), so the declaration is the check and the diagnostic
  // shows the function it answers for.
  const ast::TypeIdx ret = fn.return_type;
  if (ret.is_valid() && ast.types[ret].kind == ast::TypeKind::OwnFunc) {
    const ast::TypeFunc sig = ast.types[ret].payload.get<ast::TypeFunc>();
    rewrite(ret, sig, true, fn.name.name);
  }
  if (added.empty()) {
    return;
  }
  std::vector<ast::GenericParam> all(fn.generic.begin(), fn.generic.end());
  all.insert(all.end(), added.begin(), added.end());
  own_fn_params_.push_back(std::move(all));
  fn.generic = own_fn_params_.back();
  ast.items[item].payload.set(fn);
}

bool Checker::bind_fn_slots(u32 module,
                            std::span<const ast::GenericParam> params,
                            ast::TypeIdx declared,
                            ast::ExprIdx arg,
                            std::vector<ir::TypeIdx>& bound,
                            diag::Span span) {
  const ast::TypeNode& node = ast.types[declared];
  if (node.kind != ast::TypeKind::Func) {
    return true;
  }
  const ast::TypeFunc& shape = node.payload.get<ast::TypeFunc>();
  // The return slot lives nowhere in the receiver, only in the
  // closure body, so the first pass over arguments cannot see it.
  if (!shape.ret.is_valid() ||
      ast.types[shape.ret].kind != ast::TypeKind::Path) {
    return true;
  }
  const ast::TypePath& path = ast.types[shape.ret].payload.get<ast::TypePath>();
  if (!path.args.empty() || ast.paths[path.path].segments.size() != 1) {
    return true;
  }
  const u32 slot = param_slot(params, ast.paths[path.path].segments[0].name);
  if (slot >= params.size() || bound[slot].is_valid() ||
      is_value_param(ast, params[slot])) {
    return true;
  }
  const ir::TypeIdx actual = check_expr(module, arg, nullptr);
  if (is_error(actual)) {
    return false;
  }
  // A closure literal's own type is its nominal, whose signature is the
  // function type the literal recorded (ADR-0064); a function type
  // carries its own. Either way the return slot reads the signature.
  ir::TypeIdx sig = actual;
  if (tag_of(actual) != ir::TypeTag::Func) {
    sig = ir::TypeIdx::invalid();
    for (const CheckedModule& entry : modules) {
      for (const CheckedModule::ClosureLit& lit : entry.closures) {
        if (lit.self_type.idx == actual.idx) {
          sig = lit.func_type;
          break;
        }
      }
      if (sig.is_valid()) {
        break;
      }
    }
  }
  if (!sig.is_valid() || tag_of(sig) != ir::TypeTag::Func) {
    return true;
  }
  const ir::FuncType actual_shape =
      builder.func_types()[builder.types()[sig.idx].as_func()];
  bound[slot] = type_origin(actual_shape.ret);
  (void)span;
  return true;
}

// Instantiates a generic function or intrinsic against `args` and
// checks its body. Returns null when the signature does not match the
// intrinsic's canonical shape.

const CheckedModule::FnSig* Checker::instantiate_fn(
    u32 module,
    ast::ItemIdx item,
    const std::vector<ir::TypeIdx>& args) {
  for (const FnInstance& instance : fn_instances) {
    if (instance.module != module || instance.item != item ||
        instance.args.size() != args.size()) {
      continue;
    }
    bool same = true;
    for (usize i = 0; same && i < args.size(); ++i) {
      same = instance.args[i].idx == args[i].idx ||
             types_equal(instance.args[i], args[i]);
    }
    if (same) {
      return &modules[module].functions[instance.sig_index];
    }
  }
  const bool is_intrinsic = ast.items[item].kind == ast::ItemKind::Intrinsic;
  const std::span<const ast::GenericParam> params = fn_generic_params(item);
  const std::vector<std::pair<std::string_view, ir::TypeIdx>> kept_outer =
      type_params;
  const std::vector<ValueParam> kept_outer_values = value_params;
  type_params.clear();
  value_params.clear();
  for (usize i = 0; i < params.size() && i < args.size(); ++i) {
    if (is_value_param(ast, params[i])) {
      value_params.push_back({params[i].name.name, const_type_value(args[i]),
                              module, params[i].type});
      continue;
    }
    type_params.emplace_back(params[i].name.name, args[i]);
  }
  std::vector<ir::TypeIdx> sig_params;
  for (const ast::ItemFnParam& param : fn_params(item)) {
    sig_params.push_back(resolve_type(module, param.type, nullptr));
  }
  ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
  const ast::TypeIdx declared_ret = fn_return_type(item);
  if (declared_ret.is_valid()) {
    ret = resolve_type(module, declared_ret, nullptr);
  }
  // An intrinsic's shape is its own check, and a definition-site
  // argument that names a parameter cannot pass it: the concrete type
  // is the instantiation's (each one checks it there). ADR-0079.
  bool parametric_intrinsic = false;
  if (is_intrinsic && parametric_mode()) {
    for (const ir::TypeIdx param : sig_params) {
      if (tag_of(param) == ir::TypeTag::Param) {
        parametric_intrinsic = true;
      }
    }
    if (tag_of(ret) == ir::TypeTag::Param) {
      parametric_intrinsic = true;
    }
  }
  if (is_intrinsic && !parametric_intrinsic &&
      !check_intrinsic_signature(
          module, ast.items[item].payload.get<ast::ItemIntrinsic>(), sig_params,
          ret)) {
    type_params = kept_outer;
    value_params = kept_outer_values;
    return nullptr;
  }
  add_function(module, {.name = fn_name(item),
                        .params = sig_params,
                        .ret = ret,
                        .item = item,
                        .inst = NO_INST,
                        .is_unsafe = fn_is_unsafe(item),
                        .is_comp = fn_is_comp(item)});
  const u32 sig_index = static_cast<u32>(modules[module].functions.size()) - 1;
  // The definition-site check records under its own key: no numbering
  // slot is claimed and no body is checked here - each generic item's
  // own pass checks its body once, in its own context (ADR-0079).
  if (parametric_mode()) {
    modules[module].functions[sig_index].inst = PARAM_INST;
    fn_instances.push_back(
        FnInstance{item, module, args, sig_index, PARAM_INST});
    type_params = kept_outer;
    value_params = kept_outer_values;
    return &modules[module].functions[sig_index];
  }
  // Claim a slot in the shared instantiation numbering before checking
  // the body, so recursive calls key the same context.
  fn_instances.push_back(FnInstance{item, module, args, sig_index, NO_INST});
  const u32 inst = static_cast<u32>(inst_numbering.size());
  inst_numbering.emplace_back(base::INVALID_IDX);
  inst_by_type_.emplace(base::INVALID_IDX, inst);
  fn_instances.back().inst = inst;
  modules[module].functions[sig_index].inst = inst;

  if (!is_intrinsic) {
    const ir::TypeIdx saved_ret = fn_ret;
    const u32 saved_loop = loop_depth;
    const bool saved_in_fn = in_fn;
    const bool saved_bind = bind_comp_known;
    const u32 saved_inst = cur_inst;
    cur_inst = inst;
    check_fn(module, item, nullptr);
    cur_inst = saved_inst;
    fn_ret = saved_ret;
    loop_depth = saved_loop;
    in_fn = saved_in_fn;
    bind_comp_known = saved_bind;
  }
  // A stand-in the body answered: the signature the caller reads
  // carries the witness, and the instance's arguments follow it, so
  // the next call with the same witness finds this body check instead
  // of running another.
  if (!pending_own_fn_returns_.empty() &&
      pending_own_fn_returns_.back().item == item) {
    const PendingOwnFnReturn pending = pending_own_fn_returns_.back();
    pending_own_fn_returns_.pop_back();
    const ir::TypeIdx witness =
        own_fn_return_witness(item, pending.slot, pending.args);
    if (witness.is_valid()) {
      modules[module].functions[sig_index].ret = witness;
      for (FnInstance& instance : fn_instances) {
        if (instance.sig_index == sig_index &&
            pending.slot < instance.args.size()) {
          instance.args[pending.slot] = witness;
        }
      }
    } else {
      // No return named the signature: the error type keeps the
      // callers from meeting the stand-in, and the report says why.
      modules[module].functions[sig_index].ret = error_type();
      const u32 index = bag.emit<i18n::Key::AnalyzerOwnFunctionReturnMissing>(
          DiagCode::NotCompKnown, pending.span, fn_name(item));
      (void)index;
    }
  }
  type_params = kept_outer;
  value_params = kept_outer_values;
  return &modules[module].functions[sig_index];
}

// Binds a generic function or intrinsic's type parameters, then
// instantiates. Explicit turbofish arguments win; otherwise a parameter
// a declared parameter type pins on its own binds from that argument.
// Binds one declared parameter's slot from one argument, following the
// shape the declaration wrote: the argument inference a free function
// and a method share. `callee` names the callee in a diagnostic, and a
// slot the argument does not pin stays empty so the caller's
// cannot-infer answer stands.

bool Checker::bind_argument_slot(u32 module,
                                 const DeclaredBinding& declared,
                                 ast::ExprIdx arg,
                                 std::string_view callee,
                                 std::vector<ir::TypeIdx>& bound,
                                 diag::Span span) {
  const ir::TypeIdx actual = check_expr(module, arg, nullptr);
  if (is_error(actual)) {
    return false;
  }
  if (!declared.through_ref && !declared.through_raw) {
    // A field's type is a storage copy; the parameter binds the
    // declared type behind it rather than the copy. A slot inside a
    // generic instance reads out of that instance's own arguments;
    // when the argument names no instance the slot stays empty and
    // the check below names the turbofish the call needs.
    if (!declared.arg_at.empty()) {
      const ir::TypeIdx at = arg_at_path(type_origin(actual), declared.arg_at);
      if (at.is_valid()) {
        bound[declared.slot] = type_origin(at);
      }
      return true;
    }
    bound[declared.slot] = type_origin(actual);
    return true;
  }
  const ir::TypeTag tag = builder.types()[actual.idx].tag;
  const bool pointer_ok = declared.through_raw ? ir::is_raw_ptr_type(tag)
                                               : (tag == ir::TypeTag::Ref ||
                                                  tag == ir::TypeTag::MutRef);
  if (!pointer_ok) {
    const u32 index =
        declared.through_raw
            ? bag.emit<i18n::Key::AnalyzerTypeArgumentNeedsPointer>(
                  DiagCode::InvalidOperation, span, callee)
            : bag.emit<i18n::Key::AnalyzerTypeArgumentNeedsReference>(
                  DiagCode::InvalidOperation, span, callee);
    (void)index;
    return false;
  }
  ir::TypeIdx bound_type =
      builder.ref_types()[builder.types()[actual.idx].as_ref()].pointee;
  if (declared.through_uninit) {
    bound_type = uninit_payload(bound_type);
    if (!bound_type.is_valid()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerTypeArgumentNeedsUninit>(
          DiagCode::InvalidOperation, span, callee);
      (void)index;
      return false;
    }
  }
  if (declared.through_slice) {
    const ir::TypeTag bound_tag = builder.types()[bound_type.idx].tag;
    if (bound_tag == ir::TypeTag::Array) {
      bound_type =
          builder.array_types()[builder.types()[bound_type.idx].as_array()]
              .element;
    } else if (bound_tag == ir::TypeTag::Slice) {
      bound_type =
          builder.slice_types()[builder.types()[bound_type.idx].as_slice()]
              .element;
    } else {
      const u32 index = bag.emit<i18n::Key::AnalyzerTypeArgumentNeedsSequence>(
          DiagCode::InvalidOperation, span, callee);
      (void)index;
      return false;
    }
  }
  // A slot inside a generic instance reads out of that instance's
  // own arguments, so `&Vec<T>` binds from `&Vec<i32>`; when the
  // argument names no instance the slot stays empty and the check
  // below names the turbofish the call needs.
  if (!declared.arg_at.empty()) {
    bound_type = arg_at_path(bound_type, declared.arg_at);
    if (!bound_type.is_valid()) {
      return true;
    }
  }
  // A wrapper's payload and a slice's element are storage copies;
  // the parameter binds the declared type behind them.
  bound[declared.slot] = type_origin(bound_type);
  return true;
}

const CheckedModule::FnSig* Checker::resolve_generic_fn(
    u32 module,
    ast::ItemIdx item,
    const std::span<const ast::ExprIdx>& args,
    const std::span<const ast::TypeIdx>& explicit_args,
    diag::Span span) {
  const std::span<const ast::GenericParam> params = fn_generic_params(item);
  // A synthetic parameter of an owning `fn(...)` spelling takes its
  // witness from an argument or the body, so a turbofish spells only
  // the parameters the declaration writes (ADR-0075).
  usize spelled = 0;
  for (const ast::GenericParam& param : params) {
    if (own_fn_slot(param.name.name) == nullptr) {
      ++spelled;
    }
  }
  if (explicit_args.size() != spelled && !explicit_args.empty()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTypeArityMismatch>(
        DiagCode::ArityMismatch, span, fn_name(item), spelled,
        spelled == 1 ? "" : "s");
    (void)index;
    return nullptr;
  }
  std::vector<ir::TypeIdx> bound(params.size(), ir::TypeIdx(base::INVALID_IDX));
  usize next_explicit = 0;
  for (usize i = 0; i < params.size() && next_explicit < explicit_args.size();
       ++i) {
    if (own_fn_slot(params[i].name.name) != nullptr) {
      continue;
    }
    const ast::TypeIdx explicit_arg = explicit_args[next_explicit++];
    if (is_value_param(ast, params[i])) {
      u64 value = 0;
      if (!resolve_value_argument(module, explicit_arg, params[i].name.name,
                                  span, value)) {
        return nullptr;
      }
      bound[i] = builder.const_type(value);
      continue;
    }
    bound[i] = resolve_type(module, explicit_arg, nullptr);
  }
  for (usize i = 0; i < fn_params(item).size() && i < args.size(); ++i) {
    const DeclaredBinding declared =
        declared_binding(params, ast.types[fn_params(item)[i].type]);
    // Only a type slot binds from an argument's type; a value slot is
    // never inferred and waits for its turbofish argument.
    if (declared.slot >= params.size() ||
        is_value_param(ast, params[declared.slot]) ||
        bound[declared.slot].is_valid()) {
      continue;
    }
    if (!bind_argument_slot(module, declared, args[i], fn_name(item), bound,
                            span)) {
      return nullptr;
    }
  }
  for (usize i = 0; i < fn_params(item).size() && i < args.size(); ++i) {
    if (!bind_fn_slots(module, params, fn_params(item)[i].type, args[i], bound,
                       span)) {
      return nullptr;
    }
  }
  if (!infer_bound_arguments(module, params, bound, span)) {
    return nullptr;
  }
  // A return-position `fn()` spelling pins nothing on its own: the
  // body's first `ret` names its witness, so the signature resolves
  // with a stand-in and the body's answer replaces it after the check
  // (ADR-0075). A witness an earlier call recorded answers directly.
  std::vector<usize> return_slots;
  for (usize i = 0; i < params.size() && i < bound.size(); ++i) {
    if (bound[i].is_valid()) {
      continue;
    }
    const OwnFnSlot* declared = own_fn_slot(params[i].name.name);
    if (declared == nullptr || !declared->is_return) {
      continue;
    }
    const ir::TypeIdx known =
        own_fn_return_witness(item, i, witness_key(bound, i));
    if (known.is_valid()) {
      bound[i] = known;
      continue;
    }
    if (parametric_mode()) {
      // The witness belongs to an instantiation (ADR-0116): the
      // definition-site check leaves the slot at the error type and
      // the instantiation checks stay the authority for such items.
      bound[i] = error_type();
      continue;
    }
    bound[i] = own_fn_return_stand_in();
    return_slots.push_back(i);
  }
  // A value slot never infers, so an unfilled one is named first: the
  // type message below would suggest spelling only a type argument.
  for (usize i = 0; i < bound.size(); ++i) {
    if (!is_value_param(ast, params[i])) {
      continue;
    }
    if (bound[i].is_valid() && tag_of(bound[i]) == ir::TypeTag::Const) {
      continue;
    }
    const u32 index = bag.emit<i18n::Key::AnalyzerCannotInferValueArgument>(
        DiagCode::InvalidOperation, span, params[i].name.name, fn_name(item),
        fn_name(item));
    (void)index;
    return nullptr;
  }
  for (const ir::TypeIdx arg : bound) {
    if (!arg.is_valid()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerCannotInferTypeArguments>(
          DiagCode::InvalidOperation, span, fn_name(item), fn_name(item));
      (void)index;
      return nullptr;
    }
  }
  // ADR-0085: a type argument is Movable, whether the turbofish spelled
  // it or the arguments were inferred, and a function instantiation
  // binds its own rather than passing through the nominal path.
  if (!check_type_args_movable(bound, span)) {
    return nullptr;
  }
  std::vector<std::pair<std::string_view, ir::TypeIdx>> bounds;
  for (usize i = 0; i < params.size() && i < bound.size(); ++i) {
    bounds.emplace_back(params[i].name.name, bound[i]);
  }
  if (!check_declared_bounds(module, params, bounds, span)) {
    return nullptr;
  }
  // The body check that follows answers each fresh stand-in: the
  // instantiation consumes these records and replaces the signature's
  // return type with the witness, or reports that none came.
  for (usize slot : return_slots) {
    pending_own_fn_returns_.push_back(PendingOwnFnReturn{
        item, slot, bound[slot], own_fn_slot(params[slot].name.name), span,
        witness_key(bound, slot)});
  }
  return instantiate_fn(module, item, bound);
}

// Synthesizes one method entry for a generic instantiation and
// checks its body under the substitution. Appends before checking
// so recursive calls resolve to the in-progress entry.

const CheckedModule::MethodInfo* Checker::instantiate_method(
    u32 impl_module,
    ir::TypeIdx self_type,
    const std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
    std::span<const ValueParam> value_scope,
    const ast::ItemImpl& impl,
    std::string_view name,
    std::span<const ir::TypeIdx> method_args) {
  for (ast::ItemIdx method_item : impl.methods) {
    const ast::ItemNode& method_node = ast.items[method_item];
    if (method_node.payload.get<ast::ItemFn>().name.name != name) {
      continue;
    }
    for (const CheckedModule::MethodInfo& existing :
         modules[impl_module].methods) {
      if (existing.item == method_item &&
          existing.self_type.idx == self_type.idx &&
          std::ranges::equal(existing.method_args, method_args)) {
        return &existing;
      }
    }
    // The callee resolves its own parameters only: the caller scope
    // is hidden so a same-named parameter cannot leak through.
    std::vector<std::pair<std::string_view, ir::TypeIdx>> outer_scope =
        std::move(type_params);
    std::vector<ValueParam> outer_values = std::move(value_params);
    type_params.clear();
    value_params.clear();
    for (const auto& binding : scope) {
      type_params.push_back(binding);
    }
    for (const auto& binding : value_scope) {
      value_params.push_back(binding);
    }
    // The method's own type parameters bind from the call's explicit
    // arguments before its signature resolves.
    const std::span<const ast::GenericParam> method_params =
        fn_generic_params(method_item);
    if (method_args.size() != method_params.size()) {
      type_params = std::move(outer_scope);
      value_params = std::move(outer_values);
      return nullptr;
    }
    for (usize i = 0; i < method_params.size(); ++i) {
      type_params.emplace_back(method_params[i].name.name, method_args[i]);
    }
    std::vector<ir::TypeIdx> params;
    for (const ast::ItemFnParam& param :
         method_node.payload.get<ast::ItemFn>().params) {
      params.push_back(resolve_type(impl_module, param.type, &self_type));
    }
    ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
    if (method_node.payload.get<ast::ItemFn>().return_type.is_valid()) {
      ret = resolve_type(impl_module,
                         method_node.payload.get<ast::ItemFn>().return_type,
                         &self_type);
    }
    CheckedModule::ReceiverKind receiver = CheckedModule::ReceiverKind::None;
    if (!params.empty()) {
      receiver = classify_receiver(params[0], self_type);
    }
    add_function(impl_module, {method_node.payload.get<ast::ItemFn>().name.name,
                               params, ret, method_item})
        .is_method = true;
    const u32 method_sig_index =
        static_cast<u32>(modules[impl_module].functions.size()) - 1;
    // A method of a spec impl keeps its spec on the entry, the way a
    // concrete registration does: the body check and the call tables
    // read it.
    u32 spec_index = NO_SPEC;
    if (impl.spec.is_valid()) {
      if (const SpecEntry* entry = spec_of_impl_path(impl_module, impl.spec)) {
        spec_index = static_cast<u32>(entry - specs.data());
      }
    }
    add_method(impl_module,
               {self_type,
                method_node.payload.get<ast::ItemFn>().name.name,
                params,
                ret,
                receiver,
                method_item,
                check_drop_signature(impl_module, self_type,
                                     modules[impl_module].functions.back(),
                                     receiver, method_item),
                spec_index,
                {}});
    // Nested instantiations append during the body check; keep the
    // entry this call owns.
    CheckedModule::MethodInfo* entry = &modules[impl_module].methods.back();
    entry->method_args.assign(method_args.begin(), method_args.end());
    // The definition-site check records under its own key: no
    // numbering slot is claimed and no body is checked here (ADR-0079).
    if (parametric_mode()) {
      entry->method_inst = PARAM_INST;
      type_params = std::move(outer_scope);
      value_params = std::move(outer_values);
      return entry;
    }
    // A method with its own type arguments needs an instantiation key
    // of its own: two instances share the receiver's nominal, and the
    // side tables record a body's calls per instantiation.
    u32 inst = inst_index(self_type);
    if (inst == NO_INST) {
      // A compound receiver has no nominal instance: one number per
      // concrete self type keeps the call tables and the symbol key
      // apart from every other instantiation.
      inst = static_cast<u32>(inst_numbering.size());
      inst_numbering.emplace_back(base::INVALID_IDX);
      inst_by_type_.emplace(self_type.idx, inst);
    }
    if (!method_args.empty()) {
      inst = static_cast<u32>(inst_numbering.size());
      inst_numbering.emplace_back(base::INVALID_IDX);
    }
    entry->method_inst = inst;
    const ir::TypeIdx saved_ret = fn_ret;
    const u32 saved_loop = loop_depth;
    const bool saved_in_fn = in_fn;
    const bool saved_bind = bind_comp_known;
    const u32 saved_inst = cur_inst;
    cur_inst = inst;
    check_fn(impl_module, method_item, &self_type);
    cur_inst = saved_inst;
    fn_ret = saved_ret;
    loop_depth = saved_loop;
    in_fn = saved_in_fn;
    bind_comp_known = saved_bind;
    // A stand-in the body answered: the method's signature carries the
    // witness, and the entry's arguments follow it, so the next call
    // with the same witness finds this body check.
    if (!pending_own_fn_returns_.empty() &&
        pending_own_fn_returns_.back().item == method_item) {
      const PendingOwnFnReturn pending = pending_own_fn_returns_.back();
      pending_own_fn_returns_.pop_back();
      const ir::TypeIdx witness =
          own_fn_return_witness(method_item, pending.slot, pending.args);
      if (witness.is_valid()) {
        entry->ret = witness;
        modules[impl_module].functions[method_sig_index].ret = witness;
        if (pending.slot < entry->method_args.size()) {
          entry->method_args[pending.slot] = witness;
        }
      } else {
        entry->ret = error_type();
        modules[impl_module].functions[method_sig_index].ret = error_type();
        const u32 index = bag.emit<i18n::Key::AnalyzerOwnFunctionReturnMissing>(
            DiagCode::NotCompKnown, pending.span, name);
        (void)index;
      }
    }
    type_params = std::move(outer_scope);
    value_params = std::move(outer_values);
    return entry;
  }
  return nullptr;
}

// Synthesizes one spec method entry for a generic instantiation and
// checks its body under the substitution. The declared signature is
// the check: the impl method resolves beside it, and any mismatch is
// reported where the impl is written rather than at the call.

const ast::GenericParam* Checker::param_at(u32 index) const {
  if (index >= param_scope_.size()) {
    return nullptr;
  }
  return param_scope_[index];
}

bool Checker::parametric_mode() const {
  return cur_inst == PARAM_INST;
}

// Whether the declared parameters can stand in for the item's types: a
// value parameter has no symbolic node, and an owning `fn()` slot's
// witness is per instantiation (ADR-0116), so an item that declares
// either stays checked at its calls.

bool Checker::parametric_scope_ok(
    std::span<const ast::GenericParam> impl_params,
    std::span<const ast::GenericParam> own) const {
  for (const ast::GenericParam& param : impl_params) {
    if (is_value_param(ast, param)) {
      return false;
    }
  }
  for (const ast::GenericParam& param : own) {
    if (is_value_param(ast, param) || own_fn_slot(param.name.name) != nullptr) {
      return false;
    }
  }
  return true;
}

// Checks one generic item's body once, against its parameters: every
// declared parameter binds to the `Param` node its index names, the
// receiver resolves with them, and the records carry PARAM_INST, so
// nothing outside the pass reads what it writes. The per-instantiation
// checks keep their own records and stay the authority until the
// specialization pass consumes these bodies instead (ADR-0079).

void Checker::check_generic_body(u32 module,
                                 ast::ItemIdx item,
                                 std::span<const ast::GenericParam> impl_params,
                                 ast::TypeIdx target,
                                 bool has_target) {
  const std::span<const ast::GenericParam> own = fn_generic_params(item);
  std::vector<std::pair<std::string_view, ir::TypeIdx>> saved_types =
      std::move(type_params);
  std::vector<ValueParam> saved_values = std::move(value_params);
  type_params.clear();
  value_params.clear();
  const usize saved_scope = param_scope_.size();
  u32 slot = 0;
  for (const ast::GenericParam& param : impl_params) {
    param_scope_.push_back(&param);
    type_params.emplace_back(param.name.name, builder.param_type(slot++));
  }
  for (const ast::GenericParam& param : own) {
    param_scope_.push_back(&param);
    type_params.emplace_back(param.name.name, builder.param_type(slot++));
  }
  ir::TypeIdx self = ir::TypeIdx::invalid();
  if (has_target) {
    self = resolve_type(module, target, nullptr, true);
  }
  // Every per-body field a nested check could see is saved, the way
  // the const pre-pass does; only `cur_inst` differs, keying the
  // records where no other reader looks.
  const ir::TypeIdx saved_ret = fn_ret;
  const u32 saved_loop = loop_depth;
  const bool saved_in_fn = in_fn;
  const bool saved_bind = bind_comp_known;
  const bool saved_too_deep = reported_too_deep_;
  const u32 saved_inst = cur_inst;
  const u32 saved_comp = comp_depth;
  const usize saved_specs = spec_scope.size();
  cur_inst = PARAM_INST;
  // The comp rules of the enclosing phase do not apply: this is an
  // ordinary body check.
  comp_depth = 0;
  check_fn(module, item, has_target ? &self : nullptr);
  while (spec_scope.size() > saved_specs) {
    spec_scope.pop_back();
  }
  comp_depth = saved_comp;
  cur_inst = saved_inst;
  reported_too_deep_ = saved_too_deep;
  bind_comp_known = saved_bind;
  in_fn = saved_in_fn;
  loop_depth = saved_loop;
  fn_ret = saved_ret;
  while (param_scope_.size() > saved_scope) {
    param_scope_.pop_back();
  }
  type_params = std::move(saved_types);
  value_params = std::move(saved_values);
}

void Checker::check_generic_bodies() {
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    for (ast::ItemIdx item : tree.modules[m]->items) {
      const ast::ItemNode& node = ast.items[item];
      if (node.kind == ast::ItemKind::Fn) {
        const ast::ItemFn& fn = node.payload.get<ast::ItemFn>();
        if (fn.generic.empty() || fn.is_comp ||
            !parametric_scope_ok({}, fn.generic)) {
          continue;
        }
        check_generic_body(m, item, {}, ast::TypeIdx::invalid(), false);
        continue;
      }
      if (node.kind != ast::ItemKind::Impl) {
        continue;
      }
      const ast::ItemImpl& impl = node.payload.get<ast::ItemImpl>();
      u32 spec_index = U32_MAX;
      for (const SpecImplEntry& record : spec_impls) {
        if (record.item == item) {
          spec_index = record.spec;
          break;
        }
      }
      for (ast::ItemIdx method_item : impl.methods) {
        const ast::ItemNode& method_node = ast.items[method_item];
        if (method_node.kind != ast::ItemKind::Fn) {
          continue;
        }
        const ast::ItemFn& method = method_node.payload.get<ast::ItemFn>();
        if (impl.params.empty() && method.generic.empty()) {
          continue;
        }
        if (method.is_comp ||
            !parametric_scope_ok(impl.params, method.generic)) {
          continue;
        }
        // A spec implementation resolves its siblings through the
        // spec, the way its instantiation-time body check does.
        const usize pushed_specs = spec_scope.size();
        if (spec_index != U32_MAX) {
          spec_scope.push_back(spec_index);
        }
        check_generic_body(m, method_item, impl.params, impl.type, true);
        while (spec_scope.size() > pushed_specs) {
          spec_scope.pop_back();
        }
      }
    }
  }
}

}  // namespace analyzer
