// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/types.h"

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
#include "ast/ast.h"
#include "ast/verify.h"
#include "base/nesting.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "ir/verifier.h"

namespace analyzer {

Checker::Checker(const ModuleTree& tree,
                 ir::PointerWidth width,
                 ast::AstArena& ast,
                 diag::DiagBag& bag,
                 str::StringInterner& strings,
                 std::span<const StdHint> std_hints,
                 debug::Profiler* profiler)
    : tree(tree),
      ast(ast),
      width(width),
      bag(bag),
      std_hints(std_hints),
      profiler(profiler),
      interner(strings) {}

// Pass 1: registers every nominal definition, diagnosing duplicates
// and reserved names. No interning happens here.
void Checker::register_nominals() {
  nominals_of_module.assign(tree.modules.size(), {});
  specs_of_module.assign(tree.modules.size(), {});
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    for (ast::ItemIdx item : tree.modules[m]->items) {
      const ast::ItemNode& node = ast.items[item];
      if (node.kind != ast::ItemKind::Struct &&
          node.kind != ast::ItemKind::Enum &&
          node.kind != ast::ItemKind::Spec) {
        continue;
      }
      if (node.kind == ast::ItemKind::Spec) {
        register_spec(m, item);
        continue;
      }
      std::string_view name;
      diag::Span span;
      if (node.kind == ast::ItemKind::Struct) {
        name = node.payload.get<ast::ItemStruct>().name.name;
        span = node.payload.get<ast::ItemStruct>().name.span;
      } else {
        name = node.payload.get<ast::ItemEnum>().name.name;
        span = node.payload.get<ast::ItemEnum>().name.span;
      }
      bool duplicate = find_nominal(m, name) != nullptr;
      if (!duplicate && name == "MaybeUninit") {
        // The wrapper is compiler-owned; a declaration under the same
        // name would make the spelling resolve two ways.
        const u32 index = bag.emit<i18n::Key::AnalyzerBuiltinType>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::DuplicateDefinition, span, name);
        (void)index;
        continue;
      }
      if (!duplicate && (name == "Range" || name == "Bound") &&
          !(tree.modules[m]->is_staged &&
            is_core_package(tree.modules[m]->path))) {
        // The interval types back range expressions, so their names are
        // reserved to the package that declares them: a range always
        // constructs that one declaration.
        const u32 index = bag.emit<i18n::Key::AnalyzerBuiltinType>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::ReservedName, span, name);
        (void)index;
        continue;
      }
      if (duplicate) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::DuplicateDefinition, span, name);
        (void)index;
        continue;
      }
      nominals.push_back(NominalEntry{m, name, item, span, ir::TypeIdx(0)});
      nominals_of_module[m].push_back(static_cast<u32>(nominals.size() - 1));
    }
  }
}

NominalEntry* Checker::find_nominal(u32 module, std::string_view name) {
  if (module >= nominals_of_module.size()) {
    return nullptr;
  }
  for (u32 pos : nominals_of_module[module]) {
    NominalEntry& entry = nominals[pos];
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

SpecEntry* Checker::find_spec(u32 module, std::string_view name) {
  if (module >= specs_of_module.size()) {
    return nullptr;
  }
  for (u32 pos : specs_of_module[module]) {
    SpecEntry& entry = specs[pos];
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

std::string_view Checker::package_of(u32 module) const {
  const u32 root = tree.module_root(module);
  if (root != NO_PACKAGE_ROOT) {
    return tree.package_roots[root].identity;
  }
  const ModuleNode& node = *tree.modules[module];
  if (node.is_staged) {
    // A staged source is named `member/module`, and the member is the
    // package every module under it belongs to.
    const usize sep = node.path.find("::");
    return sep == std::string::npos
               ? std::string_view(node.path)
               : std::string_view(node.path).substr(0, sep);
  }
  return tree.package_name;
}

const PackagePolicy* Checker::policy_of(std::string_view package) const {
  for (const PackagePolicy& policy : tree.package_policies) {
    if (policy.package == package) {
      return &policy;
    }
  }
  return nullptr;
}

SpecEntry* Checker::find_spec_in_scope(u32 module, std::string_view name) {
  if (SpecEntry* entry = find_spec(module, name)) {
    return entry;
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Type || import.name != name) {
      continue;
    }
    if (SpecEntry* target = find_spec(import.target_module, import.member)) {
      return target;
    }
  }
  return nullptr;
}

bool Checker::spec_in_scope(u32 module, const SpecEntry& spec) {
  if (module >= static_cast<u32>(tree.modules.size())) {
    return false;
  }
  if (spec.module == module) {
    return true;
  }
  for (u32 scope : spec_scope) {
    if (scope < specs.size() && specs[scope].module == spec.module &&
        specs[scope].name == spec.name) {
      return true;
    }
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Type) {
      continue;
    }
    if (import.target_module == spec.module && import.member == spec.name) {
      return true;
    }
  }
  return false;
}

// A bare path `Name`, or empty when `type` is anything else.
static std::string_view bare_path_name(const ast::AstArena& ast,
                                       ast::TypeIdx type) {
  if (!type.is_valid()) {
    return {};
  }
  const ast::TypeNode& node = ast.types[type];
  if (node.kind != ast::TypeKind::Path) {
    return {};
  }
  const ast::TypePath& path = node.payload.get<ast::TypePath>();
  const std::span<const ast::Ident> segments = ast.paths[path.path].segments;
  if (segments.size() != 1 || !path.args.empty()) {
    return {};
  }
  return segments[0].name;
}

// Whether `type` is `&T` or `&mut T` over the bare path `name`.
static bool is_ref_to(const ast::AstArena& ast,
                      ast::TypeIdx type,
                      bool is_mut,
                      std::string_view name) {
  if (!type.is_valid()) {
    return false;
  }
  const ast::TypeNode& node = ast.types[type];
  if (node.kind != ast::TypeKind::Ref) {
    return false;
  }
  const ast::TypeRef& ref = node.payload.get<ast::TypeRef>();
  return ref.is_mut == is_mut && bare_path_name(ast, ref.inner) == name;
}

bool Checker::check_operator_spec(const ast::ItemSpec& declaration) {
  // The compiler owns the operator specs (ADR-0053): their declared
  // shape is part of the operator's meaning, so a staged declaration
  // that does not match the canonical shape is an error rather than a
  // different operator.
  struct Canonical {
    std::string_view spec;
    std::string_view method;
    bool mut_receiver;
  };
  static constexpr Canonical CANONICAL[] = {
      {"Index", "index", false},
      {"IndexMut", "index_mut", true},
  };
  const Canonical* canonical = nullptr;
  for (const Canonical& candidate : CANONICAL) {
    if (candidate.spec == declaration.name.name) {
      canonical = &candidate;
      break;
    }
  }
  if (canonical == nullptr) {
    return true;
  }
  const auto wrong = [&]() {
    const u32 index = bag.emit<i18n::Key::AnalyzerSpecCanonicalShape>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::SpecCanonicalShape, declaration.name.span,
        declaration.name.name);
    (void)index;
    return false;
  };
  // `<I, O>` with one method taking `&Self` and `I` and returning
  // `&O` (`&mut` on receiver and result for the mutating form).
  if (declaration.params.size() != 2 || declaration.methods.size() != 1) {
    return wrong();
  }
  const ast::SpecMethod& method = declaration.methods[0];
  if (method.name.name != canonical->method || method.is_unsafe ||
      !method.generic.empty() || method.params.size() != 2) {
    return wrong();
  }
  if (!is_ref_to(ast, method.params[0].type, canonical->mut_receiver, "Self")) {
    return wrong();
  }
  if (bare_path_name(ast, method.params[1].type) !=
      declaration.params[0].name) {
    return wrong();
  }
  return is_ref_to(ast, method.return_type, canonical->mut_receiver,
                   declaration.params[1].name);
}

u32 Checker::operator_spec(std::string_view name) const {
  for (u32 i = 0; i < static_cast<u32>(specs.size()); ++i) {
    if (specs[i].name == name && tree.is_staged_item("core", specs[i].item)) {
      return i;
    }
  }
  return U32_MAX;
}

ir::TypeIdx Checker::index_nominal(ir::TypeIdx receiver) {
  ir::TypeIdx inner = receiver;
  const ir::TypeTag tag = tag_of(inner);
  if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    inner = builder.ref_types()[builder.types()[inner.idx].as_ref()].pointee;
  }
  const ir::TypeTag inner_tag = tag_of(inner);
  if (inner_tag != ir::TypeTag::Struct && inner_tag != ir::TypeTag::Enum) {
    return error_type();
  }
  return inner;
}

void Checker::register_spec(u32 module, ast::ItemIdx item) {
  const ast::ItemNode& node = ast.items[item];
  const ast::ItemSpec& spec = node.payload.get<ast::ItemSpec>();
  if (find_nominal(module, spec.name.name) != nullptr) {
    const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::DuplicateDefinition, spec.name.span, spec.name.name);
    (void)index;
    return;
  }
  if (find_spec(module, spec.name.name) != nullptr) {
    const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::DuplicateDefinition, spec.name.span, spec.name.name);
    (void)index;
    return;
  }
  for (usize i = 0; i < spec.methods.size(); ++i) {
    for (usize j = 0; j < i; ++j) {
      if (spec.methods[j].name.name == spec.methods[i].name.name) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::DuplicateDefinition, spec.methods[i].name.span,
            spec.methods[i].name.name);
        (void)index;
        return;
      }
    }
    if (spec.methods[i].is_unsafe) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnsafeMethodUnsupported>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, spec.methods[i].name.span,
          spec.methods[i].name.name);
      (void)index;
      return;
    }
    if (!spec.methods[i].generic.empty()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecMethodTypeParameters>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::GenericArguments, spec.methods[i].name.span,
          spec.methods[i].name.name);
      (void)index;
      return;
    }
  }
  specs.push_back(SpecEntry{module, spec.name.name, item, node.span});
  specs_of_module[module].push_back(static_cast<u32>(specs.size() - 1));
  // The compiler owns the operator specs (ADR-0053): a staged core
  // declaration must keep the canonical shape. It still registers, so
  // a wrong shape is one diagnostic rather than a flood of unresolved
  // names at every use.
  if (tree.is_staged_item("core", item)) {
    (void)check_operator_spec(spec);
  }
}

bool Checker::spec_method_sig(u32 spec,
                              std::string_view name,
                              std::span<const ir::TypeIdx> spec_args,
                              ir::TypeIdx self_type,
                              u32 module,
                              std::vector<ir::TypeIdx>& params_out,
                              ir::TypeIdx& ret_out,
                              CheckedModule::ReceiverKind& receiver_out) {
  if (spec >= specs.size()) {
    return false;
  }
  const ast::ItemNode& node = ast.items[specs[spec].item];
  const ast::ItemSpec& declaration = node.payload.get<ast::ItemSpec>();
  if (spec_args.size() != declaration.params.size()) {
    return false;
  }
  for (const ast::SpecMethod& method : declaration.methods) {
    if (method.name.name != name) {
      continue;
    }
    if (!method.generic.empty()) {
      return false;
    }
    const usize pushed = type_params.size();
    for (usize i = 0; i < declaration.params.size(); ++i) {
      type_params.emplace_back(declaration.params[i].name, spec_args[i]);
    }
    params_out.clear();
    for (const ast::ItemFnParam& param : method.params) {
      params_out.push_back(resolve_type(module, param.type, &self_type));
    }
    ret_out = builder.primitive(ir::TypeTag::Void);
    if (method.return_type.is_valid()) {
      ret_out = resolve_type(module, method.return_type, &self_type);
    }
    receiver_out = CheckedModule::ReceiverKind::None;
    if (!params_out.empty()) {
      receiver_out = classify_receiver(params_out[0], self_type);
    }
    while (type_params.size() > pushed) {
      type_params.pop_back();
    }
    return true;
  }
  return false;
}

bool Checker::spec_targets_overlap(const SpecTarget& a, const SpecTarget& b) {
  if (a.nominal != b.nominal || a.args.size() != b.args.size()) {
    return false;
  }
  for (usize i = 0; i < a.args.size(); ++i) {
    if (a.args[i].is_param || b.args[i].is_param) {
      continue;
    }
    if (!types_equal(a.args[i].type, b.args[i].type)) {
      return false;
    }
  }
  return true;
}

bool Checker::spec_target_shapes_match(const SpecImplEntry& a,
                                       const SpecImplEntry& b) {
  if (a.target.nominal != b.target.nominal ||
      a.target.args.size() != b.target.args.size()) {
    return false;
  }
  // A generic impl may name its parameters differently from another, so
  // a parameter stands for the first argument position that names it:
  // `Vec<T>` and `Vec<U>` are the same shape, `Map<U, U>` and
  // `Map<U, V>` are not. Concrete arguments compare as types.
  const auto occurrence = [](const SpecTarget& target, usize at) -> usize {
    for (usize i = 0; i < at; ++i) {
      if (target.args[i].is_param &&
          target.args[i].param == target.args[at].param) {
        return i;
      }
    }
    return at;
  };
  for (usize i = 0; i < a.target.args.size(); ++i) {
    const SpecTarget::Arg& left = a.target.args[i];
    const SpecTarget::Arg& right = b.target.args[i];
    if (left.is_param != right.is_param) {
      return false;
    }
    if (!left.is_param) {
      if (!types_equal(left.type, right.type)) {
        return false;
      }
      continue;
    }
    if (occurrence(a.target, i) != occurrence(b.target, i)) {
      return false;
    }
  }
  return true;
}

void Checker::check_superspecs() {
  constexpr u32 NO_SUPER = U32_MAX;
  std::vector<u32> super_of(specs.size(), NO_SUPER);
  for (u32 i = 0; i < static_cast<u32>(specs.size()); ++i) {
    const SpecEntry& entry = specs[i];
    const ast::ItemSpec& declaration =
        ast.items[entry.item].payload.get<ast::ItemSpec>();
    if (!declaration.super.is_valid()) {
      continue;
    }
    const ast::TypeNode& super_node = ast.types[declaration.super];
    SpecEntry* target = nullptr;
    std::string_view spelling;
    if (super_node.kind == ast::TypeKind::Path) {
      const ast::TypePath& super_path = super_node.payload.get<ast::TypePath>();
      const std::span<const ast::Ident> segments =
          ast.paths[super_path.path].segments;
      if (!segments.empty()) {
        spelling = segments.back().name;
        if (!super_path.args.empty()) {
          const u32 index = bag.emit<i18n::Key::AnalyzerSpecSuperArguments>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::SpecSuperBadTarget, super_node.span, entry.name);
          (void)index;
          continue;
        }
        if (segments.size() == 1) {
          target = find_spec_in_scope(entry.module, spelling);
        } else {
          u32 super_module = NO_MODULE;
          std::string_view super_name;
          if (resolve_type_path(entry.module, super_path.path, super_module,
                                super_name)) {
            target = find_spec(super_module, super_name);
          }
        }
      }
    }
    if (target == nullptr) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecSuperUnknown>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::SpecSuperBadTarget, super_node.span, spelling);
      (void)index;
      continue;
    }
    super_of[i] = static_cast<u32>(target - specs.data());
  }
  // The super graph is functional, so a cycle is a walk that returns to
  // a spec already on its path; each closed walk reports once, for the
  // spec the back edge points at.
  std::vector<u8> state(specs.size(), 0);
  for (u32 start = 0; start < static_cast<u32>(specs.size()); ++start) {
    if (state[start] != 0) {
      continue;
    }
    std::vector<u32> path;
    u32 node = start;
    while (node != NO_SUPER && state[node] == 0) {
      state[node] = 1;
      path.push_back(node);
      node = super_of[node];
    }
    if (node != NO_SUPER && state[node] == 1) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecSuperCycle>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::SpecSuperCycle, specs[node].span, specs[node].name);
      (void)index;
    }
    for (u32 on_path : path) {
      state[on_path] = 2;
    }
  }
  // Every implementation needs an implementation of its spec's direct
  // super for the same target; the chain holds because each super's own
  // implementation owes the next.
  for (const SpecImplEntry& entry : spec_impls) {
    const u32 super = super_of[entry.spec];
    if (super == NO_SUPER) {
      continue;
    }
    bool satisfied = false;
    for (const SpecImplEntry& candidate : spec_impls) {
      if (candidate.spec == super &&
          spec_target_shapes_match(candidate, entry)) {
        satisfied = true;
        break;
      }
    }
    if (!satisfied) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecSuperMissing>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::SpecSuperMissing, entry.span, specs[entry.spec].name,
          specs[super].name);
      (void)index;
    }
  }
}

// A declaration recognized by its reserved name rather than by path.
// Range and Bound are reserved, so a tree holds at most one of each.
NominalEntry* Checker::builtin_nominal(std::string_view name) {
  for (NominalEntry& entry : nominals) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

void Checker::register_spec_impl(u32 module, ast::ItemIdx item) {
  const ast::ItemNode& node = ast.items[item];
  const ast::ItemImpl& impl = node.payload.get<ast::ItemImpl>();
  // Two methods sharing a name would both be checked against the one
  // declaration, and the second would quietly shadow the first.
  std::vector<std::string_view> method_names;
  for (ast::ItemIdx method_item : impl.methods) {
    const ast::Ident& name =
        ast.items[method_item].payload.get<ast::ItemFn>().name;
    for (std::string_view declared : method_names) {
      if (declared == name.name) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::DuplicateDefinition, name.span, name.name);
        (void)index;
        return;
      }
    }
    method_names.push_back(name.name);
  }
  // The spec side parses through the type grammar; only a path names
  // a spec. Anything else is rejected where the impl is written.
  SpecEntry* spec_entry = nullptr;
  std::string_view spec_spelling;
  const ast::TypeNode& spec_node = ast.types[impl.spec];
  if (spec_node.kind == ast::TypeKind::Path) {
    const ast::TypePath& spec_path = spec_node.payload.get<ast::TypePath>();
    const std::span<const ast::Ident> segments =
        ast.paths[spec_path.path].segments;
    if (!segments.empty()) {
      spec_spelling = segments.back().name;
      if (segments.size() == 1) {
        spec_entry = find_spec_in_scope(module, spec_spelling);
      } else {
        u32 spec_module = NO_MODULE;
        std::string_view spec_name;
        if (resolve_type_path(module, spec_path.path, spec_module, spec_name)) {
          spec_entry = find_spec(spec_module, spec_name);
        }
      }
    }
  }
  if (spec_entry == nullptr) {
    const u32 index = bag.emit<i18n::Key::AnalyzerUnresolved>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
        spec_node.span, "spec", spec_spelling);
    (void)index;
    return;
  }
  const u32 spec_index = static_cast<u32>(spec_entry - specs.data());
  // ADR-0053: a spec its package seals to a suite may be implemented
  // only by that package or by a package of that suite. The manifest
  // name list is the seal; the reserved implementing-side key that
  // would open it is not accepted yet.
  if (const PackagePolicy* const declaring =
          policy_of(package_of(spec_entry->module));
      declaring != nullptr) {
    bool sealed = false;
    for (std::string_view sealed_name : declaring->suite_only) {
      if (sealed_name == spec_entry->name) {
        sealed = true;
        break;
      }
    }
    if (sealed) {
      const PackagePolicy* const implementing = policy_of(package_of(module));
      const bool same_package = implementing != nullptr &&
                                implementing->package == declaring->package;
      const bool same_suite = implementing != nullptr &&
                              !declaring->suite.empty() &&
                              implementing->suite == declaring->suite;
      if (!same_package && !same_suite) {
        const u32 index = bag.emit<i18n::Key::AnalyzerSpecSealed>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::SpecSealed,
            spec_node.span, spec_entry->name, declaring->package);
        (void)index;
        return;
      }
    }
  }
  const ast::ItemSpec& declaration =
      ast.items[spec_entry->item].payload.get<ast::ItemSpec>();
  const bool generic_impl = !impl.params.empty();
  // Spec arguments bind the spec's parameters; a generic impl names
  // its own parameters directly, exactly like an inherent target.
  // Spec arguments bind the spec's parameters. A generic impl may name
  // one of its own parameters or a concrete type, so `Index<usize, T>`
  // is expressible; the shapes persist for call-site instantiation.
  std::vector<SpecTarget::Arg> spec_arg_shapes;
  std::vector<ir::TypeIdx> spec_args;
  {
    const ast::TypePath& spec_path = spec_node.payload.get<ast::TypePath>();
    if (spec_path.args.size() != declaration.params.size()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerTypeArityMismatch>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityMismatch,
          spec_node.span, spec_entry->name, declaration.params.size(),
          declaration.params.size() == 1 ? "" : "s");
      (void)index;
      return;
    }
    for (ast::TypeIdx arg : spec_path.args) {
      bool direct = false;
      std::string_view param;
      if (generic_impl) {
        const ast::TypeNode& arg_node = ast.types[arg];
        if (arg_node.kind == ast::TypeKind::Path) {
          const ast::Path& path =
              ast.paths[arg_node.payload.get<ast::TypePath>().path];
          if (path.segments.size() == 1 &&
              arg_node.payload.get<ast::TypePath>().args.empty()) {
            for (const ast::Ident& candidate : impl.params) {
              if (candidate.name == path.segments[0].name) {
                direct = true;
                param = candidate.name;
                break;
              }
            }
          }
        }
      }
      if (direct) {
        spec_arg_shapes.push_back(SpecTarget::Arg{true, param, error_type()});
        continue;
      }
      const ir::TypeIdx resolved = resolve_type(module, arg, nullptr);
      if (is_error(resolved)) {
        return;
      }
      spec_arg_shapes.push_back(SpecTarget::Arg{false, {}, resolved});
      if (!generic_impl) {
        spec_args.push_back(resolved);
      }
    }
  }
  // The target is a nominal type in scope; v1 never implements a
  // spec for a compound or reference type.
  NominalEntry* target_entry = nullptr;
  const ast::TypeNode& target_node = ast.types[impl.type];
  if (target_node.kind == ast::TypeKind::Path) {
    const ast::TypePath& target_path = target_node.payload.get<ast::TypePath>();
    const std::span<const ast::Ident> segments =
        ast.paths[target_path.path].segments;
    if (!segments.empty()) {
      if (segments.size() == 1) {
        target_entry = find_nominal_in_scope(module, segments.back().name);
      } else {
        u32 target_module = NO_MODULE;
        std::string_view target_name;
        if (resolve_type_path(module, target_path.path, target_module,
                              target_name)) {
          target_entry = find_nominal(target_module, target_name);
        }
      }
    }
  }
  if (target_entry == nullptr) {
    const u32 index = bag.emit<i18n::Key::AnalyzerSpecRequiresNominalTarget>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
        target_node.span);
    (void)index;
    return;
  }
  SpecTarget target;
  target.nominal = nominal_index(target_entry);
  ir::TypeIdx self_type = error_type();
  {
    const ast::TypePath& target_path = target_node.payload.get<ast::TypePath>();
    if (target_path.args.size() != nominal_params(*target_entry).size()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerTypeArityMismatch>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityMismatch,
          target_node.span, target_entry->name,
          nominal_params(*target_entry).size(),
          nominal_params(*target_entry).size() == 1 ? "" : "s");
      (void)index;
      return;
    }
    if (generic_impl) {
      for (ast::TypeIdx arg : target_path.args) {
        const ast::TypeNode& arg_node = ast.types[arg];
        bool direct = false;
        std::string_view param;
        if (arg_node.kind == ast::TypeKind::Path) {
          const ast::Path& path =
              ast.paths[arg_node.payload.get<ast::TypePath>().path];
          if (path.segments.size() == 1 &&
              arg_node.payload.get<ast::TypePath>().args.empty()) {
            for (const ast::Ident& candidate : impl.params) {
              if (candidate.name == path.segments[0].name) {
                direct = true;
                param = candidate.name;
                break;
              }
            }
          }
        }
        if (!direct) {
          const u32 index = bag.emit<i18n::Key::AnalyzerSpecTargetNotDirect>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::GenericArguments, arg_node.span);
          (void)index;
          return;
        }
        target.args.push_back(SpecTarget::Arg{true, param, error_type()});
      }
    } else {
      std::vector<ir::TypeIdx> args;
      for (ast::TypeIdx arg : target_path.args) {
        const ir::TypeIdx resolved = resolve_type(module, arg, nullptr);
        if (is_error(resolved)) {
          return;
        }
        args.push_back(resolved);
        target.args.push_back(SpecTarget::Arg{false, {}, resolved});
      }
      // A non-generic nominal interns; instantiating would mint a
      // second type the call site never names.
      if (args.empty()) {
        self_type = intern_nominal(*target_entry);
      } else {
        self_type = instantiate_generic(target.nominal, args, target_node.span);
      }
      if (is_error(self_type)) {
        return;
      }
    }
  }
  for (const SpecImplEntry& existing : spec_impls) {
    if (existing.spec == spec_index &&
        spec_targets_overlap(existing.target, target)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerConflictingSpecImpl>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::DuplicateDefinition, node.span, spec_entry->name,
          target_entry->name);
      (void)index;
      return;
    }
  }
  // Method presence is syntactic, so generic impls check it now and
  // everything else waits for a concrete self.
  for (ast::ItemIdx method_item : impl.methods) {
    const ast::ItemNode& method_node = ast.items[method_item];
    const std::string_view name =
        method_node.payload.get<ast::ItemFn>().name.name;
    if (!method_node.payload.get<ast::ItemFn>().generic.empty()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecMethodTypeParameters>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::GenericArguments,
          method_node.payload.get<ast::ItemFn>().name.span, name);
      (void)index;
      return;
    }
    bool declared = false;
    for (const ast::SpecMethod& method : declaration.methods) {
      if (method.name.name == name) {
        declared = true;
        break;
      }
    }
    if (!declared) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecUnknownMethod>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          method_node.payload.get<ast::ItemFn>().name.span, name,
          spec_entry->name);
      (void)index;
      return;
    }
  }
  for (const ast::SpecMethod& method : declaration.methods) {
    bool implemented = false;
    for (ast::ItemIdx method_item : impl.methods) {
      if (ast.items[method_item].payload.get<ast::ItemFn>().name.name ==
          method.name.name) {
        implemented = true;
        break;
      }
    }
    if (!implemented) {
      const u32 index = bag.emit<i18n::Key::AnalyzerMissingSpecMethod>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
          node.span, method.name.name, spec_entry->name);
      (void)index;
      return;
    }
  }
  if (generic_impl) {
    spec_impls.push_back(SpecImplEntry{spec_index, module, std::move(target),
                                       std::move(spec_arg_shapes), item,
                                       node.span});
    return;
  }
  for (ast::ItemIdx method_item : impl.methods) {
    const ast::ItemNode& method_node = ast.items[method_item];
    const std::string_view name =
        method_node.payload.get<ast::ItemFn>().name.name;
    std::vector<ir::TypeIdx> declared_params;
    ir::TypeIdx declared_ret = error_type();
    CheckedModule::ReceiverKind declared_receiver =
        CheckedModule::ReceiverKind::None;
    if (!spec_method_sig(spec_index, name, spec_args, self_type, module,
                         declared_params, declared_ret, declared_receiver)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecUnknownMethod>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          method_node.payload.get<ast::ItemFn>().name.span, name,
          spec_entry->name);
      (void)index;
      return;
    }
    std::vector<ir::TypeIdx> params;
    for (const ast::ItemFnParam& param :
         method_node.payload.get<ast::ItemFn>().params) {
      params.push_back(resolve_type(module, param.type, &self_type));
    }
    ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
    if (method_node.payload.get<ast::ItemFn>().return_type.is_valid()) {
      ret = resolve_type(module,
                         method_node.payload.get<ast::ItemFn>().return_type,
                         &self_type);
    }
    CheckedModule::ReceiverKind receiver = CheckedModule::ReceiverKind::None;
    if (!params.empty()) {
      receiver = classify_receiver(params[0], self_type);
    }
    bool matches = params.size() == declared_params.size() &&
                   receiver == declared_receiver &&
                   types_equal(ret, declared_ret);
    for (usize i = 0; matches && i < params.size(); ++i) {
      matches = types_equal(params[i], declared_params[i]);
    }
    if (!matches) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecSignatureMismatch>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
          method_node.span, name, spec_entry->name);
      (void)index;
      return;
    }
    CheckedModule::FnSig& sig =
        add_function(module, {name, params, ret, method_item});
    sig.is_method = true;
    add_method(module, {self_type, name, sig.params, sig.ret, receiver,
                        method_item, false, spec_index});
  }
  spec_impls.push_back(SpecImplEntry{spec_index, module, std::move(target),
                                     std::move(spec_arg_shapes), item,
                                     node.span});
}

bool Checker::record_inherent_method(u32 target_module,
                                     std::string_view target_name,
                                     std::string_view method) {
  return inherent_methods_
      .insert(InherentMethod{target_module, target_name, method})
      .second;
}

u32 Checker::find_child_module(u32 module, std::string_view name) const {
  if (module >= children_by_tail_.size()) {
    return NO_MODULE;
  }
  const auto found = children_by_tail_[module].find(name);
  return found == children_by_tail_[module].end() ? NO_MODULE : found->second;
}

ir::TypeIdx Checker::primitive_type(ast::PrimitiveKind kind, diag::Span span) {
  using PK = ast::PrimitiveKind;
  using TT = ir::TypeTag;
  switch (kind) {
    case PK::I8: return builder.primitive(TT::I8);
    case PK::I16: return builder.primitive(TT::I16);
    case PK::I32: return builder.primitive(TT::I32);
    case PK::I64: return builder.primitive(TT::I64);
    case PK::Isize:
      return builder.primitive(width == ir::PointerWidth::W64 ? TT::I64
                                                              : TT::I32);
    case PK::U8: return builder.primitive(TT::U8);
    case PK::U16: return builder.primitive(TT::U16);
    case PK::U32: return builder.primitive(TT::U32);
    case PK::U64: return builder.primitive(TT::U64);
    case PK::Usize:
      return builder.primitive(width == ir::PointerWidth::W64 ? TT::U64
                                                              : TT::U32);
    case PK::F32: return builder.primitive(TT::F32);
    case PK::F64: return builder.primitive(TT::F64);
    case PK::Bool: return builder.primitive(TT::I1);
    case PK::Str: return builder.primitive(TT::Str);
    case PK::I128:
    case PK::U128: {
      const u32 index = bag.emit<i18n::Key::AnalyzerTypeNotSupported>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::UnsupportedType, span);
      (void)index;
      return error_type();
    }
  }
}

ir::TypeIdx Checker::error_type() {
  return builder.error_type();
}

// Derives the receiver kind from a resolved first parameter: exactly
// Self, &Self, or &mut Self count; anything else is an associated
// function regardless of the parameter name.
CheckedModule::ReceiverKind Checker::classify_receiver(ir::TypeIdx first,
                                                       ir::TypeIdx self) {
  if (first.idx == self.idx) {
    return CheckedModule::ReceiverKind::ByValue;
  }
  const ir::TypeNode& node = builder.types()[first];
  if (node.tag == ir::TypeTag::Ref &&
      builder.ref_types()[node.as_ref()].pointee.idx == self.idx) {
    return CheckedModule::ReceiverKind::Shared;
  }
  if (node.tag == ir::TypeTag::MutRef &&
      builder.ref_types()[node.as_ref()].pointee.idx == self.idx) {
    return CheckedModule::ReceiverKind::Exclusive;
  }
  return CheckedModule::ReceiverKind::None;
}

// `MaybeUninit<T>` is a compiler-owned one-field struct standing for
// storage that holds a `T` nobody has written yet. It shares the
// payload's layout and lowers transparently to it, so the wrapper costs
// nothing; its purpose is to keep the unwritten value out of reach
// until `uninit_assume` releases it.
ir::TypeIdx Checker::intern_uninit(ir::TypeIdx payload) {
  // Wrappers are keyed on the type the payload was copied from, so a
  // payload reached through a chain of storage copies resolves to the
  // same wrapper as the type it came from.
  const ir::TypeIdx key = type_origin(payload);
  for (const auto& [wrapper, cached] : uninit_types_) {
    if (cached == key) {
      return wrapper;
    }
  }
  const ir::TypeIdx wrapper = builder.struct_type(
      uninit_name_id,
      [&] {
        ir::TypeSeq seq;
        seq.push(storage_copy(key));
        return seq.finish();
      }(),
      ir::TypeIdxRange{});
  uninit_types_.emplace_back(wrapper, key);
  return wrapper;
}

ir::TypeIdx Checker::uninit_payload(ir::TypeIdx type) const {
  if (tag_of(type) != ir::TypeTag::Struct) {
    return ir::TypeIdx::invalid();
  }
  const ir::StructType& shape =
      builder.struct_types()[builder.types()[type.idx].as_struct()];
  if (shape.name != uninit_name_id) {
    return ir::TypeIdx::invalid();
  }
  return shape.fields.size() == 1 ? shape.fields[0] : ir::TypeIdx::invalid();
}

// Interns a registered nominal, reserving its index first so recursive
// references resolve to it. A post-pass rejects uninhabited cycles.
// Interns a name, or reports the shared table as spent once and returns an
// invalid id. The table is shared with lowering and codegen, so one report
// covers the run; the sweep that saw it stops rather than interning more
// names into a table it knows is full.
str::StringPoolId Checker::intern_name(std::string_view name) {
  if (const std::optional<str::StringPoolId> id = interner.try_intern(name);
      id.has_value()) {
    return *id;
  }
  if (!name_table_exhausted_) {
    name_table_exhausted_ = true;
    const u32 index = bag.emit<i18n::Key::AnalyzerNameTableExhausted>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::NameTableExhausted, diag::Span{});
    (void)index;
  }
  return str::INVALID_STRING_POOL_ID;
}

ir::TypeIdx Checker::intern_nominal(NominalEntry& entry) {
  if (entry.complete || entry.started) {
    return entry.type;
  }
  const str::StringPoolId name = intern_name(entry.name);
  if (name == str::INVALID_STRING_POOL_ID) {
    return error_type();
  }
  const ast::ItemNode& node = ast.items[entry.item];
  const bool is_struct = node.kind == ast::ItemKind::Struct;
  // Every variant name is interned before the node is reserved, so a spent
  // table leaves nothing half-built.
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
  entry.type =
      is_struct ? builder.reserve_struct(name) : builder.reserve_enum(name);
  entry.started = true;
  if (is_struct) {
    std::vector<ir::TypeIdx> fields;
    fields.reserve(node.payload.get<ast::ItemStruct>().fields.size());
    for (const ast::ItemStructField& field :
         node.payload.get<ast::ItemStruct>().fields) {
      // Exclusive references are legal fields; the structural Copy
      // rule marks the aggregate move-only.
      fields.push_back(resolve_type(entry.module, field.type, nullptr));
    }
    ir::TypeSeq seq;
    for (ir::TypeIdx field : fields) {
      seq.push(storage_copy(field));
    }
    builder.fill_struct(entry.type, seq.finish(), ir::TypeIdxRange{});
  } else {
    // Payloads resolve first so variant nodes append back-to-back.
    std::vector<std::vector<ir::TypeIdx>> payloads;
    payloads.reserve(node.payload.get<ast::ItemEnum>().variants.size());
    for (const ast::ItemEnumVariant& variant :
         node.payload.get<ast::ItemEnum>().variants) {
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
    builder.fill_enum(entry.type, variants.finish(), ir::TypeIdxRange{});
  }
  entry.complete = true;
  // Whatever kind it is: a structure whose fields are being read, an
  // enumeration whose variants are being matched, a type a method is looked up
  // on, or one a diagnostic has to name.
  nominal_by_type_.emplace(entry.type.idx,
                           static_cast<u32>(&entry - nominals.data()));
  return entry.type;
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

ir::TypeIdx Checker::storage_copy(ir::TypeIdx type) {
  const ir::TypeIdx copy = builder.ref_type(type);
  type_origins_.emplace_back(copy, type);
  type_origins_by_index_[copy.idx] = type.idx;
  return copy;
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

std::span<const ast::Ident> Checker::nominal_params(const NominalEntry& entry) {
  const ast::ItemNode& node = ast.items[entry.item];
  if (node.kind == ast::ItemKind::Struct) {
    return node.payload.get<ast::ItemStruct>().params;
  }
  return node.payload.get<ast::ItemEnum>().params;
}

usize Checker::push_generic_scope(const GenericInstance& instance) {
  const usize kept = type_params.size();
  const std::span<const ast::Ident> params =
      nominal_params(nominals[instance.nominal]);
  for (usize i = 0; i < params.size(); ++i) {
    type_params.emplace_back(params[i].name, instance.args[i]);
  }
  return kept;
}

void Checker::pop_generic_scope(usize kept) {
  while (type_params.size() > kept) {
    type_params.pop_back();
  }
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
      if (decl.params[p].name == path.segments[0].name) {
        slot = p;
        break;
      }
    }
    if (slot == decl.params.size()) {
      continue;
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
  const std::span<const ast::Ident> params = nominal_params(nominals[nominal]);
  if (args.size() != params.size()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTypeArityMismatch>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityMismatch,
        span, nominals[nominal].name, params.size(),
        params.size() == 1 ? "" : "s");
    (void)index;
    return error_type();
  }
  // A function type in a type argument has no symbol encoding yet, so two
  // instantiations that differ only there would share a mangled name.
  // Refusing is the whole rule until the encoding lands.
  for (const ir::TypeIdx arg : args) {
    if (is_error(arg) || tag_of(arg) != ir::TypeTag::Func) {
      continue;
    }
    const u32 index =
        bag.emit<i18n::Key::AnalyzerFunctionTypeAsGenericArgument>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::UnsupportedType, span);
    (void)index;
    return error_type();
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
  if (is_struct) {
    const std::span<const ast::Ident> params =
        node.payload.get<ast::ItemStruct>().params;
    for (usize i = 0; i < params.size(); ++i) {
      type_params.emplace_back(params[i].name, args[i]);
    }
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
    for (usize i = 0; i < decl.params.size(); ++i) {
      type_params.emplace_back(decl.params[i].name, args[i]);
    }
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
  // A field type can instantiate another generic nominal, which pushes
  // entries of its own; this instance's entry is the one recorded above.
  GenericInstance& instance = generic_instances[instance_index];
  instance.started = true;
  instance.complete = true;
  (void)span;
  return reserved;
}

// Resolves all path segments but the last to a module. Shared by type
// and value paths; `what` names the namespace for diagnostics.
bool Checker::walk_module_prefix(u32 module,
                                 ast::PathIdx path,
                                 std::string_view what,
                                 u32& module_out) {
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  const std::string_view head = segments[0].name;
  if (segments.size() == 1) {
    module_out = module;
    return true;
  }
  u32 current = NO_MODULE;
  // The head opens a package when it spells one of the tree's
  // package roots, which wins over a module of the same name: a
  // dependency is reached by its identity, and a same-named
  // module by `package::` or `self::`.
  u32 opened_root = analyzer::NO_PACKAGE_ROOT;
  if (head == "package") {
    current = tree.root;
  } else if (head == "self") {
    current = module;
  } else if (head == "super") {
    if (parents[module] == NO_MODULE) {
      const u32 index = bag.emit<i18n::Key::AnalyzerRootModuleHasNoParent>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
          ast.paths[path].span);
      (void)index;
      return false;
    }
    current = parents[module];
  } else {
    const auto root = root_by_identity_.find(head);
    if (root != root_by_identity_.end()) {
      current = tree.package_roots[root->second].module;
      opened_root = root->second;
    }
    if (opened_root == analyzer::NO_PACKAGE_ROOT) {
      current = find_child_module(module, head);
      if (current == NO_MODULE) {
        const u32 index = bag.emit<i18n::Key::AnalyzerUnresolved>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
            ast.paths[path].span, what, head);
        (void)index;
        return false;
      }
    }
  }
  for (usize i = 1; i + 1 < segments.size(); ++i) {
    current = find_child_module(current, segments[i].name);
    if (current == NO_MODULE) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnresolved>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
          ast.paths[path].span, what, segments[i].name);
      (void)index;
      return false;
    }
  }
  // A path that crosses into a dependency reaches only that
  // package's `[modules] export` list, and the module the path
  // walked to is the surface the boundary trims. A path from
  // inside the dependency stays inside it.
  if (opened_root != analyzer::NO_PACKAGE_ROOT) {
    std::string module_path;
    for (usize i = 1; i + 1 < segments.size(); ++i) {
      if (!module_path.empty()) {
        module_path.push_back('/');
      }
      module_path.append(segments[i].name);
    }
    if (!module_path.empty() &&
        emit_withheld_module(tree.package_roots, tree.module_roots, module,
                             opened_root, module_path, ast.paths[path].span,
                             bag)) {
      return false;
    }
  }
  module_out = current;
  return true;
}

// Resolves a type path to its defining module and member name.
bool Checker::resolve_type_path(u32 module,
                                ast::PathIdx path,
                                u32& module_out,
                                std::string_view& name_out) {
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.empty()) {
    return false;
  }
  if (!walk_module_prefix(module, path, "type", module_out)) {
    return false;
  }
  name_out = segments.back().name;
  return true;
}

ir::TypeIdx Checker::resolve_type(u32 module,
                                  ast::TypeIdx type,
                                  const ir::TypeIdx* self,
                                  bool behind_ref) {
  if (nesting_.exhausted()) {
    report_too_deep(ast.types[type].span);
    return error_type();
  }
  const base::NestingScope scope(nesting_);
  const ast::TypeNode& node = ast.types[type];
  switch (node.kind) {
    case ast::TypeKind::Primitive: {
      return primitive_type(node.payload.get<ast::TypePrimitive>().primitive,
                            node.span);
    }
    case ast::TypeKind::Unit: return builder.primitive(ir::TypeTag::Void);
    case ast::TypeKind::Never: return builder.never_type();
    case ast::TypeKind::Str: return builder.primitive(ir::TypeTag::Str);
    case ast::TypeKind::Tuple: {
      std::vector<ir::TypeIdx> elements;
      elements.reserve(node.payload.get<ast::TypeTuple>().elements.size());
      for (ast::TypeIdx element : node.payload.get<ast::TypeTuple>().elements) {
        elements.push_back(resolve_type(module, element, self));
      }
      ir::TypeSeq seq;
      for (ir::TypeIdx element : elements) {
        // Origin-recorded like every other slot copy, so structural
        // equality sees through the contiguity copies.
        seq.push(storage_copy(element));
      }
      return builder.tuple_type(seq.finish());
    }
    case ast::TypeKind::Array: {
      const ast::TypeArray& array = node.payload.get<ast::TypeArray>();
      const ir::TypeIdx element = resolve_type(module, array.element, self);
      return builder.array_type(element, array.count);
    }
    case ast::TypeKind::Ref: {
      ir::TypeIdx pointee = resolve_type(
          module, node.payload.get<ast::TypeRef>().inner, self, true);
      return builder.reference_type(pointee,
                                    node.payload.get<ast::TypeRef>().is_mut);
    }
    case ast::TypeKind::RawPtr: {
      const ast::TypeRawPtr& ptr = node.payload.get<ast::TypeRawPtr>();
      // Raw pointers are thin, so an unsized pointee has no
      // representation. Resolving it would refuse it as not behind a
      // reference, which is the wrong guidance for a pointer.
      if (ast.types[ptr.inner].kind == ast::TypeKind::Slice) {
        const u32 index = bag.emit<i18n::Key::AnalyzerRawPointerNeedsSized>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
            node.span);
        (void)index;
        return error_type();
      }
      const ir::TypeIdx pointee = resolve_type(module, ptr.inner, self, false);
      return builder.raw_pointer_type(pointee, ptr.is_mut);
    }
    case ast::TypeKind::Func: {
      const ast::TypeFunc& func = node.payload.get<ast::TypeFunc>();
      ir::TypeSeq seq;
      for (ast::TypeIdx param : func.params) {
        seq.push(storage_copy(resolve_type(module, param, self)));
      }
      return builder.func_type(
          seq.finish(), storage_copy(resolve_type(module, func.ret, self)));
    }
    case ast::TypeKind::Slice: {
      const ast::TypeSlice& slice = node.payload.get<ast::TypeSlice>();
      const ir::TypeIdx element = resolve_type(module, slice.element, self);
      if (!behind_ref) {
        const u32 index =
            bag.emit<i18n::Key::AnalyzerUnsizedTypeNeedsReference>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::UnknownType, node.span);
        (void)index;
        return error_type();
      }
      return builder.slice_type(element);
    }
    case ast::TypeKind::Path: {
      const ast::Path& path = ast.paths[node.payload.get<ast::TypePath>().path];
      if (path.segments.size() == 1) {
        const std::string_view name = path.segments[0].name;
        if (name == "Self") {
          if (self == nullptr) {
            const u32 index = bag.emit<i18n::Key::AnalyzerSelfOutsideImpl>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::UnknownType, node.span);
            (void)index;
            return error_type();
          }
          if (!node.payload.get<ast::TypePath>().args.empty()) {
            const u32 index =
                bag.emit<i18n::Key::AnalyzerGenericArgumentsUnsupported>(
                    diag::Severity::Error, diag::Stage::Analyzer,
                    DiagCode::GenericArguments, node.span);
            (void)index;
            return error_type();
          }
          return *self;
        }
        // Type parameters shadow everything but `Self`.
        for (usize i = type_params.size(); i > 0; --i) {
          if (type_params[i - 1].first == name) {
            if (!node.payload.get<ast::TypePath>().args.empty()) {
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerGenericArgumentsUnsupported>(
                      diag::Severity::Error, diag::Stage::Analyzer,
                      DiagCode::GenericArguments, node.span);
              (void)index;
              return error_type();
            }
            return type_params[i - 1].second;
          }
        }
        if (name == "MaybeUninit") {
          const auto& args = node.payload.get<ast::TypePath>().args;
          if (args.size() != 1) {
            const u32 index = bag.emit<i18n::Key::AnalyzerMaybeUninitArity>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::GenericArguments, node.span);
            (void)index;
            return error_type();
          }
          return intern_uninit(resolve_type(module, args[0], self));
        }
      }
      u32 target_module = NO_MODULE;
      std::string_view target_name;
      if (!resolve_type_path(module, node.payload.get<ast::TypePath>().path,
                             target_module, target_name)) {
        return error_type();
      }
      // Single-segment names resolve through imports as well.
      NominalEntry* entry = find_nominal(target_module, target_name);
      if (entry == nullptr && path.segments.size() == 1) {
        for (const Import& import : tree.modules[module]->imports) {
          if (import.ns != Namespace::Type || import.name != target_name) {
            continue;
          }
          NominalEntry* target =
              find_nominal(import.target_module, import.member);
          if (target != nullptr) {
            entry = target;
            break;
          }
        }
      }
      if (entry == nullptr) {
        const std::string_view package =
            std_hint_package(std_hints, target_name);
        if (!package.empty()) {
          const u32 index = bag.emit<i18n::Key::AnalyzerTypeInStandardLibrary>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::UnknownType, node.span, target_name, target_name,
              package);
          (void)index;
          return error_type();
        }
        const u32 index = bag.emit<i18n::Key::AnalyzerUnknownType>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
            node.span, target_name);
        (void)index;
        return error_type();
      }
      const usize param_count = nominal_params(*entry).size();
      const usize arg_count = node.payload.get<ast::TypePath>().args.size();
      if (param_count == 0) {
        if (arg_count != 0) {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerGenericArgumentsUnsupported>(
                  diag::Severity::Error, diag::Stage::Analyzer,
                  DiagCode::GenericArguments, node.span);
          (void)index;
          return error_type();
        }
        return intern_nominal(*entry);
      }
      if (arg_count != param_count) {
        const u32 index = bag.emit<i18n::Key::AnalyzerArityMismatch>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::ArityMismatch, node.span, target_name, param_count,
            param_count == 1 ? "" : "s");
        (void)index;
        return error_type();
      }
      std::vector<ir::TypeIdx> args;
      args.reserve(arg_count);
      for (ast::TypeIdx arg : node.payload.get<ast::TypePath>().args) {
        args.push_back(resolve_type(module, arg, self));
      }
      const u32 nominal = static_cast<u32>(entry - nominals.data());
      return instantiate_generic(nominal, args, node.span);
    }
  }
}

// Closed compiler-known intrinsic set (see docs/spec/items.md).
// `print`/`println`/`panic` stay callable without a declaration
// until core provides them; `memcopy` requires one.
bool Checker::is_known_intrinsic(std::string_view name) {
  return name == "memcopy" || name == "print" || name == "println" ||
         name == "panic" || name == "str_len" || name == "str_byte" ||
         name == "str_slice" || name == "sys_write" ||
         name == "str_from_parts" || name == "slice_len" ||
         name == "slice_from_parts" || name == "slice_from_parts_mut" ||
         name == "alloc" || name == "dealloc" || name == "elem_ptr" ||
         name == "elem_ref" || name == "size_of" || name == "align_of" ||
         name == "uninit_write" || name == "uninit_assume" ||
         name == "uninit_ref" || name == "ptr_offset" ||
         name == "ptr_offset_mut";
}

// The intrinsics whose preconditions the compiler cannot check
// (ADR-0050): the declaration must carry `unsafe` and a call needs
// the gate. Every other intrinsic is safe whatever its arguments.
static bool intrinsic_is_unsafe(std::string_view name) {
  return name == "alloc" || name == "dealloc" || name == "elem_ptr" ||
         name == "elem_ref" || name == "memcopy" || name == "uninit_assume" ||
         name == "str_from_parts" || name == "slice_from_parts" ||
         name == "slice_from_parts_mut" || name == "ptr_offset" ||
         name == "ptr_offset_mut";
}

// What may cross a C boundary in the first ABI slice (ADR-0051):
// scalars, raw pointers, and `()` as a return. Everything else is
// refused rather than lowered optimistically.
static bool extern_abi_type_ok(ir::TypeTag tag, bool is_return) {
  if (is_return && tag == ir::TypeTag::Void) {
    return true;
  }
  return is_integer_type(tag) || is_float_type(tag) || is_raw_ptr_type(tag);
}

// Verifies a declared intrinsic signature against its canonical
// shape; declarations are documentation-checked, never trusted.
bool Checker::check_intrinsic_signature(u32 module,
                                        const ast::ItemIntrinsic& intrinsic,
                                        const std::vector<ir::TypeIdx>& params,
                                        ir::TypeIdx ret) {
  const std::string_view name = intrinsic.name.name;
  // Unsafety is part of the canonical shape: a precondition the
  // compiler cannot check must be declared, and one it can check must
  // not be, so the declaration and the gate cannot drift.
  if (intrinsic_is_unsafe(name) != intrinsic.is_unsafe) {
    const u32 index =
        intrinsic.is_unsafe
            ? bag.emit<i18n::Key::AnalyzerIntrinsicUnexpectedUnsafe>(
                  diag::Severity::Error, diag::Stage::Analyzer,
                  DiagCode::InvalidOperation, intrinsic.name.span, name)
            : bag.emit<i18n::Key::AnalyzerIntrinsicMissingUnsafe>(
                  diag::Severity::Error, diag::Stage::Analyzer,
                  DiagCode::InvalidOperation, intrinsic.name.span, name);
    (void)index;
    return false;
  }
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const ir::TypeIdx u8 = builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx usize_ty = builder.primitive(
      width == ir::PointerWidth::W64 ? ir::TypeTag::U64 : ir::TypeTag::U32);
  const ir::TypeIdx isize_ty = builder.primitive(
      width == ir::PointerWidth::W64 ? ir::TypeTag::I64 : ir::TypeTag::I32);
  std::vector<ir::TypeIdx> expected;
  ir::TypeIdx expected_ret = builder.primitive(ir::TypeTag::Void);
  const auto wrong = [&]() {
    const u32 index = bag.emit<i18n::Key::AnalyzerIntrinsicSignature>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::InvalidOperation, intrinsic.name.span, name);
    (void)index;
    return false;
  };
  const auto is_usize = [&](ir::TypeIdx type) {
    return builder.types()[type.idx].tag == builder.types()[usize_ty.idx].tag;
  };
  const auto is_isize = [&](ir::TypeIdx type) {
    return builder.types()[type.idx].tag == builder.types()[isize_ty.idx].tag;
  };
  const auto pointee = [&](ir::TypeIdx type) {
    return builder.ref_types()[builder.types()[type.idx].as_ref()].pointee;
  };
  // Generic intrinsics leave their type parameters free, so their
  // shapes are checked structurally rather than against fixed types.
  const auto or_wrong = [&](bool ok) { return ok ? true : wrong(); };
  // Buffer element types are `MaybeUninit<T>`, so the heap intrinsics
  // agree on the wrapper rather than on a concrete element type.
  const auto uninit_slot = [&](ir::TypeIdx type) {
    return builder.types()[type.idx].tag == ir::TypeTag::MutRef &&
           uninit_payload(pointee(type)).is_valid();
  };
  // A shared borrow of a wrapper slot: what `elem_ref` and `uninit_ref`
  // take, so a buffer is readable through a shared owner.
  const auto shared_uninit_slot = [&](ir::TypeIdx type) {
    return builder.types()[type.idx].tag == ir::TypeTag::Ref &&
           uninit_payload(pointee(type)).is_valid();
  };
  // A wrapper's payload is a storage copy, so comparing it against a
  // declared type compares the types the copies came from.
  const auto same = [&](ir::TypeIdx a, ir::TypeIdx b) {
    return type_origin(a).idx == type_origin(b).idx;
  };
  if (name == "alloc") {
    // `alloc<T>(count: usize) -> &mut MaybeUninit<T>`.
    return or_wrong(params.size() == 1 && is_usize(params[0]) &&
                    builder.types()[ret.idx].tag == ir::TypeTag::MutRef);
  }
  if (name == "dealloc") {
    // `dealloc<T>(ptr: &mut MaybeUninit<T>, count: usize)`.
    return or_wrong(params.size() == 2 && uninit_slot(params[0]) &&
                    is_usize(params[1]) &&
                    builder.types()[ret.idx].tag == ir::TypeTag::Void);
  }
  if (name == "size_of" || name == "align_of") {
    return or_wrong(params.empty() && is_usize(ret));
  }
  if (name == "elem_ptr") {
    // `elem_ptr<T>(ptr: &mut MaybeUninit<T>, index: usize) -> &mut
    // MaybeUninit<T>`.
    return or_wrong(params.size() == 2 && uninit_slot(params[0]) &&
                    is_usize(params[1]) &&
                    builder.types()[ret.idx].tag == ir::TypeTag::MutRef &&
                    pointee(params[0]).idx == pointee(ret).idx);
  }
  if (name == "elem_ref") {
    // `elem_ref<T>(ptr: &MaybeUninit<T>, index: usize) -> &
    // MaybeUninit<T>`. The shared counterpart of `elem_ptr`, so a buffer
    // is readable through a shared borrow of its owner.
    return or_wrong(params.size() == 2 && shared_uninit_slot(params[0]) &&
                    is_usize(params[1]) &&
                    builder.types()[ret.idx].tag == ir::TypeTag::Ref &&
                    same(pointee(params[0]), pointee(ret)));
  }
  if (name == "uninit_ref") {
    // `uninit_ref<T>(slot: &MaybeUninit<T>) -> &T`. The shared
    // counterpart of `uninit_assume`, with the same caveat: reading
    // before anything was written yields whatever was there.
    return or_wrong(params.size() == 1 && shared_uninit_slot(params[0]) &&
                    builder.types()[ret.idx].tag == ir::TypeTag::Ref &&
                    same(uninit_payload(pointee(params[0])), pointee(ret)));
  }
  if (name == "uninit_write") {
    // `uninit_write<T>(slot: &mut MaybeUninit<T>, value: T)`.
    return or_wrong(params.size() == 2 && uninit_slot(params[0]) &&
                    same(uninit_payload(pointee(params[0])), params[1]) &&
                    builder.types()[ret.idx].tag == ir::TypeTag::Void);
  }
  if (name == "uninit_assume") {
    // `uninit_assume<T>(slot: &mut MaybeUninit<T>) -> &mut T`.
    return or_wrong(params.size() == 1 && uninit_slot(params[0]) &&
                    builder.types()[ret.idx].tag == ir::TypeTag::MutRef &&
                    same(uninit_payload(pointee(params[0])), pointee(ret)));
  }
  if (name == "ptr_offset" || name == "ptr_offset_mut") {
    // `ptr_offset<T>(ptr: *T, count: isize) -> *T`, and the exclusive
    // counterpart over `*mut T`. The count is signed so one spelling
    // moves both ways; offsetting is an operation the gate covers.
    const ir::TypeTag want =
        name == "ptr_offset" ? ir::TypeTag::RawPtr : ir::TypeTag::RawMutPtr;
    return or_wrong(
        params.size() == 2 && builder.types()[params[0].idx].tag == want &&
        is_isize(params[1]) && builder.types()[ret.idx].tag == want &&
        same(pointee(params[0]), pointee(ret)));
  }
  // A slice reference of either kind: what `slice_len` reads and
  // what the slice constructors return through.
  const auto slice_ref = [&](ir::TypeIdx type) {
    const ir::TypeTag tag = builder.types()[type.idx].tag;
    return tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef;
  };
  if (name == "slice_len") {
    // `slice_len<T>(s: &[T]) -> usize`.
    return or_wrong(params.size() == 1 && slice_ref(params[0]) &&
                    tag_of(pointee(params[0])) == ir::TypeTag::Slice &&
                    is_usize(ret));
  }
  if (name == "slice_from_parts" || name == "slice_from_parts_mut") {
    // `slice_from_parts<T>(ptr: &T, len: usize) -> &[T]`, and the
    // exclusive counterpart returning `&mut [T]`.
    const ir::TypeTag ret_tag = builder.types()[ret.idx].tag;
    const ir::TypeTag ptr_tag = params.size() == 2
                                    ? builder.types()[params[0].idx].tag
                                    : ir::TypeTag::Error;
    if (!slice_ref(ret) ||
        (ptr_tag != ir::TypeTag::Ref && ptr_tag != ir::TypeTag::MutRef)) {
      return wrong();
    }
    if (tag_of(pointee(ret)) != ir::TypeTag::Slice) {
      return wrong();
    }
    if (tag_of(pointee(params[0])) == ir::TypeTag::Slice) {
      return wrong();
    }
    const ir::SliceType& slice =
        builder.slice_types()[builder.types()[pointee(ret)].as_slice()];
    const bool kinds_match =
        (ret_tag == ir::TypeTag::MutRef && ptr_tag == ir::TypeTag::MutRef) ||
        (ret_tag == ir::TypeTag::Ref && ptr_tag == ir::TypeTag::Ref);
    return or_wrong(params.size() == 2 && is_usize(params[1]) &&
                    same(slice.element, pointee(params[0])) && kinds_match);
  }
  if (name == "memcopy") {
    expected.push_back(builder.reference_type(u8, true));
    expected.push_back(builder.reference_type(u8, false));
    expected.push_back(usize_ty);
  } else if (name == "print" || name == "println") {
    expected.push_back(str);
  } else if (name == "panic") {
    expected.push_back(str);
    expected_ret = builder.never_type();
  } else if (name == "str_len") {
    expected.push_back(str);
    expected_ret = usize_ty;
  } else if (name == "str_byte") {
    expected.push_back(str);
    expected.push_back(usize_ty);
    expected_ret = u8;
  } else if (name == "str_slice") {
    expected.push_back(str);
    expected.push_back(usize_ty);
    expected.push_back(usize_ty);
    expected_ret = str;
  } else if (name == "sys_write") {
    expected.push_back(builder.primitive(ir::TypeTag::I32));
    expected.push_back(str);
  } else if (name == "str_from_parts") {
    expected.push_back(builder.reference_type(u8, false));
    expected.push_back(usize_ty);
    expected_ret = str;
  } else {
    return wrong();
  }
  if (params.size() != expected.size() || ret.idx != expected_ret.idx) {
    return wrong();
  }
  for (usize i = 0; i < params.size(); ++i) {
    if (params[i].idx != expected[i].idx) {
      return wrong();
    }
  }
  (void)module;
  return true;
}

void Checker::process_module(u32 module) {
  // The value namespace of this module: functions, intrinsics, statics,
  // and constants share it, and a second declaration under one name
  // would resolve to whichever entry a lookup reached first.
  std::vector<std::string_view> value_names;
  const auto value_taken = [&value_names](std::string_view name) {
    for (std::string_view declared : value_names) {
      if (declared == name) {
        return true;
      }
    }
    return false;
  };
  const auto duplicate_value = [this](const ast::Ident& name) {
    const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::DuplicateDefinition, name.span, name.name);
    (void)index;
  };
  for (ast::ItemIdx item : tree.modules[module]->items) {
    const ast::ItemNode& node = ast.items[item];
    switch (node.kind) {
      case ast::ItemKind::Struct:
      case ast::ItemKind::Enum: {
        NominalEntry* entry = nullptr;
        if (node.kind == ast::ItemKind::Struct) {
          entry = find_nominal(module,
                               node.payload.get<ast::ItemStruct>().name.name);
        } else {
          entry =
              find_nominal(module, node.payload.get<ast::ItemEnum>().name.name);
        }
        if (entry != nullptr) {
          // Generic declarations intern per instantiation on use; the
          // bare declaration has no type of its own.
          if (!nominal_params(*entry).empty()) {
            break;
          }
          const ir::TypeIdx resolved = intern_nominal(*entry);
          modules[module].types.push_back({entry->name, resolved});
          if (node.kind == ast::ItemKind::Struct) {
            std::vector<std::string_view> fields;
            for (const ast::ItemStructField& field :
                 node.payload.get<ast::ItemStruct>().fields) {
              fields.push_back(field.name.name);
            }
            modules[module].structs.push_back({resolved, std::move(fields)});
          } else if (node.kind == ast::ItemKind::Enum) {
            std::vector<std::string_view> variants;
            for (const ast::ItemEnumVariant& variant :
                 node.payload.get<ast::ItemEnum>().variants) {
              variants.push_back(variant.name.name);
            }
            modules[module].enums.push_back(
                {entry->name, resolved, std::move(variants)});
          }
        }
      } break;
      case ast::ItemKind::Fn: {
        const ast::Ident& name = node.payload.get<ast::ItemFn>().name;
        if (value_taken(name.name)) {
          duplicate_value(name);
          break;
        }
        value_names.push_back(name.name);
        // Generic functions register per instantiation at their call
        // sites; eager registration cannot bind their parameters.
        if (!node.payload.get<ast::ItemFn>().generic.empty()) {
          break;
        }
        std::vector<ir::TypeIdx> params;
        for (const ast::ItemFnParam& param :
             node.payload.get<ast::ItemFn>().params) {
          params.push_back(resolve_type(module, param.type, nullptr));
        }
        ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
        if (node.payload.get<ast::ItemFn>().return_type.is_valid()) {
          ret = resolve_type(
              module, node.payload.get<ast::ItemFn>().return_type, nullptr);
        }
        const ast::ItemFn& fn = node.payload.get<ast::ItemFn>();
        add_function(module, {.name = fn.name.name,
                              .params = std::move(params),
                              .ret = ret,
                              .item = item,
                              .is_unsafe = fn.is_unsafe});
        break;
      }
      case ast::ItemKind::Intrinsic: {
        const ast::ItemIntrinsic& intrinsic =
            node.payload.get<ast::ItemIntrinsic>();
        if (value_taken(intrinsic.name.name)) {
          duplicate_value(intrinsic.name);
          break;
        }
        value_names.push_back(intrinsic.name.name);
        if (!is_known_intrinsic(intrinsic.name.name)) {
          const u32 index = bag.emit<i18n::Key::AnalyzerUnknownIntrinsic>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::UnknownIntrinsic, intrinsic.name.span,
              intrinsic.name.name);
          (void)index;
          break;
        }
        // A generic intrinsic registers per instantiation at its call
        // sites, but its shape is still checked here: binding each
        // parameter to a placeholder resolves the declaration to a
        // concrete signature the structural check can read.
        if (!intrinsic.generic.empty()) {
          const std::vector<std::pair<std::string_view, ir::TypeIdx>> kept =
              type_params;
          type_params.clear();
          for (const ast::Ident& param : intrinsic.generic) {
            type_params.emplace_back(param.name,
                                     builder.primitive(ir::TypeTag::U8));
          }
          std::vector<ir::TypeIdx> shape_params;
          for (const ast::ItemFnParam& param : intrinsic.params) {
            shape_params.push_back(resolve_type(module, param.type, nullptr));
          }
          ir::TypeIdx shape_ret = builder.primitive(ir::TypeTag::Void);
          if (intrinsic.return_type.is_valid()) {
            shape_ret = resolve_type(module, intrinsic.return_type, nullptr);
          }
          type_params = kept;
          check_intrinsic_signature(module, intrinsic, shape_params, shape_ret);
          break;
        }
        std::vector<ir::TypeIdx> params;
        for (const ast::ItemFnParam& param : intrinsic.params) {
          if (param.is_comp) {
            const u32 index =
                bag.emit<i18n::Key::AnalyzerCompParameterOnIntrinsic>(
                    diag::Severity::Error, diag::Stage::Analyzer,
                    DiagCode::InvalidComp, ast.patterns[param.pattern].span);
            (void)index;
            break;
          }
          params.push_back(resolve_type(module, param.type, nullptr));
        }
        ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
        if (intrinsic.return_type.is_valid()) {
          ret = resolve_type(module, intrinsic.return_type, nullptr);
        }
        if (!check_intrinsic_signature(module, intrinsic, params, ret)) {
          break;
        }
        add_function(module, {.name = intrinsic.name.name,
                              .params = std::move(params),
                              .ret = ret,
                              .item = item,
                              .is_unsafe = intrinsic.is_unsafe});
        break;
      }
      case ast::ItemKind::Extern: {
        // ADR-0051: a declaration names a symbol the linker resolves,
        // calling it is an operation the gate covers, and the ABI is
        // scalars, raw pointers, and `()` so every shape the compiler
        // cannot lower honestly is refused here.
        const ast::ItemExtern& block = node.payload.get<ast::ItemExtern>();
        for (const ast::ItemExternFn& fn : block.fns) {
          if (value_taken(fn.name.name)) {
            duplicate_value(fn.name);
            continue;
          }
          value_names.push_back(fn.name.name);
          if (!fn.generic.empty()) {
            const u32 index = bag.emit<i18n::Key::AnalyzerExternGeneric>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::GenericArguments, fn.name.span, fn.name.name);
            (void)index;
            continue;
          }
          std::vector<ir::TypeIdx> params;
          bool abi_ok = true;
          for (const ast::ItemFnParam& param : fn.params) {
            if (param.is_comp) {
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerExternCompParameter>(
                      diag::Severity::Error, diag::Stage::Analyzer,
                      DiagCode::InvalidComp, ast.patterns[param.pattern].span);
              (void)index;
              abi_ok = false;
              break;
            }
            const ir::TypeIdx type = resolve_type(module, param.type, nullptr);
            if (!is_error(type) && !extern_abi_type_ok(tag_of(type), false)) {
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerExternTypeNotSupported>(
                      diag::Severity::Error, diag::Stage::Analyzer,
                      DiagCode::UnsupportedType, ast.types[param.type].span,
                      pretty_tag(tag_of(type)));
              (void)index;
              abi_ok = false;
              break;
            }
            params.push_back(type);
          }
          ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
          if (abi_ok && fn.return_type.is_valid()) {
            ret = resolve_type(module, fn.return_type, nullptr);
            if (!is_error(ret) && !extern_abi_type_ok(tag_of(ret), true)) {
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerExternTypeNotSupported>(
                      diag::Severity::Error, diag::Stage::Analyzer,
                      DiagCode::UnsupportedType, ast.types[fn.return_type].span,
                      pretty_tag(tag_of(ret)));
              (void)index;
              abi_ok = false;
            }
          }
          if (!abi_ok) {
            continue;
          }
          add_function(module, {.name = fn.name.name,
                                .params = std::move(params),
                                .ret = ret,
                                .item = item,
                                .is_unsafe = true,
                                .is_extern = true});
        }
        break;
      }
      case ast::ItemKind::Static:
      case ast::ItemKind::Const: {
        const bool is_const = node.kind == ast::ItemKind::Const;
        const ast::Ident name = is_const
                                    ? node.payload.get<ast::ItemConst>().name
                                    : node.payload.get<ast::ItemStatic>().name;
        if (value_taken(name.name)) {
          duplicate_value(name);
          break;
        }
        value_names.push_back(name.name);
        const ast::TypeIdx type =
            is_const ? node.payload.get<ast::ItemConst>().type
                     : node.payload.get<ast::ItemStatic>().type;
        const ast::ExprIdx init =
            is_const ? node.payload.get<ast::ItemConst>().init
                     : node.payload.get<ast::ItemStatic>().init;
        modules[module].statics.push_back(
            {name.name, resolve_type(module, type, nullptr), init, is_const});
        break;
      }
      case ast::ItemKind::Impl: {
        // Unsafe methods wait for their own slice; the marker parses so
        // the refusal is about the feature, not about the syntax.
        for (ast::ItemIdx method : node.payload.get<ast::ItemImpl>().methods) {
          const ast::ItemFn& fn = ast.items[method].payload.get<ast::ItemFn>();
          if (!fn.is_unsafe) {
            continue;
          }
          const u32 index =
              bag.emit<i18n::Key::AnalyzerUnsafeMethodUnsupported>(
                  diag::Severity::Error, diag::Stage::Analyzer,
                  DiagCode::InvalidOperation, fn.name.span, fn.name.name);
          (void)index;
        }
        if (node.payload.get<ast::ItemImpl>().spec.is_valid()) {
          register_spec_impl(module, item);
          break;
        }
        ir::TypeIdx self_type = error_type();
        bool self_ok = false;
        // Generic impls instantiate per method call; their methods
        // wait for instantiation-time checking.
        bool generic_impl = !node.payload.get<ast::ItemImpl>().params.empty();
        // The nominal the block attaches to, when its path resolved; the
        // method registry checks across blocks through it.
        u32 target_module = NO_MODULE;
        std::string_view target_name;
        bool target_known = false;
        const ast::TypeNode& self_node =
            ast.types[node.payload.get<ast::ItemImpl>().type];
        if (self_node.kind == ast::TypeKind::Path) {
          if (!self_node.payload.get<ast::TypePath>().args.empty() &&
              node.payload.get<ast::ItemImpl>().params.empty()) {
            const u32 index =
                bag.emit<i18n::Key::AnalyzerGenericImplUnsupported>(
                    diag::Severity::Error, diag::Stage::Analyzer,
                    DiagCode::GenericArguments, self_node.span);
            (void)index;
          } else {
            if (resolve_type_path(module,
                                  self_node.payload.get<ast::TypePath>().path,
                                  target_module, target_name)) {
              NominalEntry* entry = find_nominal(target_module, target_name);
              if (entry != nullptr) {
                target_known = true;
                if (nominal_params(*entry).empty() &&
                    node.payload.get<ast::ItemImpl>().params.empty()) {
                  self_type = intern_nominal(*entry);
                  self_ok = true;
                } else {
                  generic_impl = true;
                }
              } else {
                const u32 index =
                    bag.emit<i18n::Key::AnalyzerInherentImplNeedsNominalType>(
                        diag::Severity::Error, diag::Stage::Analyzer,
                        DiagCode::UnknownType, self_node.span);
                (void)index;
              }
            }
          }
        } else {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerInherentImplNeedsNominalType>(
                  diag::Severity::Error, diag::Stage::Analyzer,
                  DiagCode::UnknownType, self_node.span);
          (void)index;
        }
        for (ast::ItemIdx method : node.payload.get<ast::ItemImpl>().methods) {
          const ast::ItemNode& method_node = ast.items[method];
          // A second method under a name the nominal already carries in
          // an inherent impl would shadow the first for one receiver
          // shape; the registry spans blocks and modules, and generic
          // blocks are on record here before their bodies defer.
          if (target_known &&
              !record_inherent_method(
                  target_module, target_name,
                  method_node.payload.get<ast::ItemFn>().name.name)) {
            duplicate_value(method_node.payload.get<ast::ItemFn>().name);
            continue;
          }
          if (generic_impl) {
            continue;
          }
          std::vector<ir::TypeIdx> params;
          for (const ast::ItemFnParam& param :
               method_node.payload.get<ast::ItemFn>().params) {
            params.push_back(resolve_type(module, param.type,
                                          self_ok ? &self_type : nullptr));
          }
          ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
          if (method_node.payload.get<ast::ItemFn>().return_type.is_valid()) {
            ret = resolve_type(
                module, method_node.payload.get<ast::ItemFn>().return_type,
                self_ok ? &self_type : nullptr);
          }
          CheckedModule::FnSig& sig = add_function(
              module, {method_node.payload.get<ast::ItemFn>().name.name,
                       std::move(params), ret, method});
          sig.is_method = true;
          CheckedModule::ReceiverKind receiver =
              CheckedModule::ReceiverKind::None;
          if (self_ok && !sig.params.empty()) {
            receiver = classify_receiver(sig.params[0], self_type);
          }
          add_method(module,
                     {self_ok ? self_type : error_type(),
                      method_node.payload.get<ast::ItemFn>().name.name,
                      sig.params, sig.ret, receiver, method,
                      self_ok && check_drop_signature(module, self_type, sig,
                                                      receiver, method)});
        }
        break;
      }
      case ast::ItemKind::Spec: break;
      case ast::ItemKind::Use: break;
    }
  }
}

bool Checker::check_drop_signature(u32 module,
                                   ir::TypeIdx self_type,
                                   const CheckedModule::FnSig& sig,
                                   CheckedModule::ReceiverKind receiver,
                                   ast::ItemIdx item) {
  static constexpr std::string_view DROP = "drop";
  if (sig.name != DROP || receiver == CheckedModule::ReceiverKind::None) {
    return false;
  }
  const ast::Ident& name = ast.items[item].payload.get<ast::ItemFn>().name;
  const bool valid = receiver == CheckedModule::ReceiverKind::ByValue &&
                     sig.params.size() == 1 &&
                     tag_of(sig.ret) == ir::TypeTag::Void;
  if (!valid) {
    const u32 index = bag.emit<i18n::Key::AnalyzerBadDestructorSignature>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::BadDropSignature, name.span);
    (void)index;
    return false;
  }
  if (ir::is_copy_type(builder.state(), self_type)) {
    // `Copy` is structural, so a Copy type has no owned resource for a
    // destructor to release; a copy of it would be dropped too.
    const u32 index = bag.emit<i18n::Key::AnalyzerCopyTypeCannotHaveDestructor>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::DropOnCopy,
        name.span);
    (void)index;
    return false;
  }
  (void)module;
  return true;
}

void Checker::resolve_drops() {
  drop_glue_.clear();
  needs_drop_.clear();
  std::vector<u32> stack;
  // Resolving a generic destructor instantiates it, which can intern
  // further types, so the scan follows the type table as it grows.
  for (u32 i = 0; i < builder.types().size(); ++i) {
    drop_scan(ir::TypeIdx(i), stack);
  }
}

bool Checker::drop_scan(ir::TypeIdx type, std::vector<u32>& stack) {
  if (needs_drop_.size() <= type.idx) {
    drop_glue_.resize(type.idx + 1, CheckedModule::DropGlue{});
    needs_drop_.resize(type.idx + 1, false);
  }
  if (needs_drop_[type.idx]) {
    return true;
  }
  // A struct's field range holds storage copies so it stays contiguous,
  // and a copy is a distinct type index. Destructors are declared on the
  // type the copy came from, so the scan follows the copy back.
  const ir::TypeIdx origin = type_origin(type);
  if (origin.idx != type.idx) {
    const bool result = drop_scan(origin, stack);
    drop_glue_[type.idx] = drop_glue_[origin.idx];
    needs_drop_[type.idx] = result;
    return result;
  }
  for (u32 entry : stack) {
    // A type that contains itself has no finite destructor walk. The
    // cycle check rejects such a type before this runs.
    if (entry == type.idx) {
      return false;
    }
  }
  stack.push_back(type.idx);
  const ir::TypeNode& node = builder.types()[type];
  bool result = false;
  CheckedModule::DropGlue glue;
  switch (node.tag) {
    case ir::TypeTag::Struct:
    case ir::TypeTag::Enum: {
      glue = find_drop_glue(type);
      if (glue.index != base::INVALID_IDX) {
        result = true;
        break;
      }
      // No destructor of its own: ending a value still runs the
      // destructors of whatever it holds.
      result = holds_destructible(type, stack);
      break;
    }
    case ir::TypeTag::Array:
      result = drop_scan(builder.array_types()[node.as_array()].element, stack);
      break;
    default: break;
  }
  stack.pop_back();
  needs_drop_[type.idx] = result;
  if (glue.index != base::INVALID_IDX) {
    drop_glue_[type.idx] = glue;
  }
  return result;
}

CheckedModule::MethodInfo& Checker::add_method(u32 module,
                                               CheckedModule::MethodInfo info) {
  CheckedModule& target = modules[module];
  target.methods.push_back(std::move(info));
  const CheckedModule::MethodInfo& method = target.methods.back();
  const u32 position = static_cast<u32>(target.methods.size() - 1);
  methods_by_self_[method.self_type.idx].emplace_back(module, position);
  position_by_address_.emplace(&method, std::pair<u32, u32>{module, position});
  if (!method.is_drop) {
    return target.methods.back();
  }
  const CheckedModule::DropGlue glue{module, position};
  const u32 key = method.self_type.idx;
  const auto found = drop_glue_by_type_.find(key);
  if (found == drop_glue_by_type_.end() || found->second.module > glue.module ||
      (found->second.module == glue.module &&
       found->second.index > glue.index)) {
    drop_glue_by_type_[key] = glue;
  }
  return target.methods.back();
}

CheckedModule::DropGlue Checker::find_drop_glue(ir::TypeIdx type) {
  const auto found = drop_glue_by_type_.find(type.idx);
  if (found != drop_glue_by_type_.end()) {
    return found->second;
  }
  // A generic type reaches its destructor through `impl<T> Name<T>`,
  // which only exists once instantiated.
  const CheckedModule::MethodInfo* method =
      lookup_method(type, "drop", NO_MODULE, diag::Span{}, false);
  if (method == nullptr) {
    return {};
  }
  // Where the method lives is what the address table holds. This walked every
  // method in the package to find the one it already had, once per type asked
  // about, so a package paid its methods for each of its types.
  const auto position = position_by_address_.find(method);
  if (position == position_by_address_.end()) {
    return {};
  }
  return {position->second.first, position->second.second};
}

bool Checker::holds_destructible(ir::TypeIdx type, std::vector<u32>& stack) {
  const ir::TypeNode& node = builder.types()[type];
  if (node.tag == ir::TypeTag::Struct) {
    for (ir::TypeIdx field : builder.struct_types()[node.as_struct()].fields) {
      if (drop_scan(field, stack)) {
        return true;
      }
    }
    return false;
  }
  for (ir::EnumVariantTypeIdx v :
       builder.enum_types()[node.as_enum()].variants) {
    for (ir::TypeIdx payload : builder.enum_variant_types()[v].fields) {
      if (drop_scan(payload, stack)) {
        return true;
      }
    }
  }
  return false;
}

bool Checker::has_value_cycle(ir::TypeIdx root,
                              std::vector<ir::TypeIdx>& stack,
                              const ir::Storage& storage) {
  for (ir::TypeIdx entry : stack) {
    if (entry.idx == root.idx) {
      return true;
    }
  }
  const ir::TypeNode& node = storage.types()[root];
  stack.push_back(root);
  bool cycle = false;
  switch (node.tag) {
    case ir::TypeTag::Struct: {
      const ir::StructType& struct_type =
          storage.struct_types()[node.as_struct()];
      for (ir::TypeIdx field : struct_type.fields) {
        if (has_value_cycle(field, stack, storage)) {
          cycle = true;
          break;
        }
      }
      break;
    }
    case ir::TypeTag::Enum: {
      const ir::EnumType& enum_type = storage.enum_types()[node.as_enum()];
      for (ir::EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = ir::EnumVariantTypeIdx(vidx.idx + 1)) {
        const ir::EnumVariantType& variant = storage.enum_variant_types()[vidx];
        for (ir::TypeIdx field : variant.fields) {
          if (has_value_cycle(field, stack, storage)) {
            cycle = true;
            break;
          }
        }
        if (cycle) {
          break;
        }
      }
      break;
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType& tuple = storage.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : tuple.elements) {
        if (has_value_cycle(element, stack, storage)) {
          cycle = true;
          break;
        }
      }
      break;
    }
    case ir::TypeTag::Array: {
      const ir::ArrayType& array = storage.array_types()[node.as_array()];
      cycle = has_value_cycle(array.element, stack, storage);
      break;
    }
    case ir::TypeTag::Slice: {
      const ir::SliceType& slice = storage.slice_types()[node.as_slice()];
      cycle = has_value_cycle(slice.element, stack, storage);
      break;
    }
    default: break;
  }
  stack.pop_back();
  return cycle;
}

std::string_view Checker::nominal_name(ir::TypeIdx idx) const {
  const auto declaring = nominal_by_type_.find(idx.idx);
  if (declaring != nominal_by_type_.end()) {
    return nominals[declaring->second].name;
  }
  for (const GenericInstance& instance : generic_instances) {
    if (instance.type.idx == idx.idx) {
      return nominals[instance.nominal].name;
    }
  }
  return "type";
}

// Expression checking

ir::TypeTag Checker::tag_of(ir::TypeIdx idx) const {
  return builder.types()[idx].tag;
}

bool Checker::is_integer_tag(ir::TypeTag tag) const {
  switch (tag) {
    case ir::TypeTag::I8:
    case ir::TypeTag::I16:
    case ir::TypeTag::I32:
    case ir::TypeTag::I64:
    case ir::TypeTag::U8:
    case ir::TypeTag::U16:
    case ir::TypeTag::U32:
    case ir::TypeTag::U64: return true;
    default: return false;
  }
}

bool Checker::is_float_tag(ir::TypeTag tag) const {
  return tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64;
}

bool Checker::is_void(ir::TypeIdx idx) const {
  return tag_of(idx) == ir::TypeTag::Void;
}

bool Checker::is_never(ir::TypeIdx idx) const {
  return tag_of(idx) == ir::TypeTag::Never;
}

bool Checker::is_error(ir::TypeIdx idx) const {
  return tag_of(idx) == ir::TypeTag::Error;
}

// User-facing type names: the table spells bool as `i1` and unit as
// `void`, which read poorly in diagnostics.
const char* Checker::pretty_tag(ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::Void: return "()";
    case ir::TypeTag::I1: return "bool";
    case ir::TypeTag::Never: return "!";
    case ir::TypeTag::Str: return "str";
    default: return ir::type_to_str(tag);
  }
}

// Structural type equality. Field slots hold storage copies, so index
// equality under-compares: both sides normalize to their origin first,
// primitives compare by tag, references/tuples/arrays recurse, and
// nominals compare by index. Every cycle passes through a nominal, but a
// seen-pair set guards regardless.
bool Checker::types_equal(ir::TypeIdx a, ir::TypeIdx b) {
  std::vector<u64> seen;
  return types_equal_inner(type_origin(a), type_origin(b), seen);
}

bool Checker::types_equal_inner(ir::TypeIdx a,
                                ir::TypeIdx b,
                                std::vector<u64>& seen) {
  // Slot copies nest: tuple and array elements are copies of their own,
  // so each level normalizes before comparing, not just the top one.
  a = type_origin(a);
  b = type_origin(b);
  if (a.idx == b.idx) {
    return true;
  }
  const u64 key = (static_cast<u64>(a.idx) << 32) | static_cast<u64>(b.idx);
  for (u64 prior : seen) {
    if (prior == key) {
      return true;
    }
  }
  seen.push_back(key);
  const ir::TypeTag ta = tag_of(a);
  const ir::TypeTag tb = tag_of(b);
  if (ta != tb) {
    return false;
  }
  switch (ta) {
    case ir::TypeTag::Ref:
    case ir::TypeTag::MutRef:
    case ir::TypeTag::RawPtr:
    case ir::TypeTag::RawMutPtr: {
      const ir::TypeIdx pa =
          builder.ref_types()[builder.types()[a].as_ref()].pointee;
      const ir::TypeIdx pb =
          builder.ref_types()[builder.types()[b].as_ref()].pointee;
      return types_equal_inner(pa, pb, seen);
    }
    case ir::TypeTag::Array: {
      const ir::ArrayType& aa =
          builder.array_types()[builder.types()[a].as_array()];
      const ir::ArrayType& ab =
          builder.array_types()[builder.types()[b].as_array()];
      return aa.count == ab.count &&
             types_equal_inner(aa.element, ab.element, seen);
    }
    case ir::TypeTag::Slice: {
      const ir::SliceType& sa =
          builder.slice_types()[builder.types()[a].as_slice()];
      const ir::SliceType& sb =
          builder.slice_types()[builder.types()[b].as_slice()];
      return types_equal_inner(sa.element, sb.element, seen);
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType& ta_t =
          builder.tuple_types()[builder.types()[a].as_tuple()];
      const ir::TupleType& tb_t =
          builder.tuple_types()[builder.types()[b].as_tuple()];
      if (ta_t.elements.size() != tb_t.elements.size()) {
        return false;
      }
      for (u32 i = 0; i < ta_t.elements.size(); ++i) {
        if (!types_equal_inner(ta_t.elements[i], tb_t.elements[i], seen)) {
          return false;
        }
      }
      return true;
    }
    case ir::TypeTag::Func: {
      const ir::FuncType& fa =
          builder.func_types()[builder.types()[a].as_func()];
      const ir::FuncType& fb =
          builder.func_types()[builder.types()[b].as_func()];
      if (fa.params.size() != fb.params.size()) {
        return false;
      }
      for (u32 i = 0; i < fa.params.size(); ++i) {
        if (!types_equal_inner(fa.params[i], fb.params[i], seen)) {
          return false;
        }
      }
      return types_equal_inner(fa.ret, fb.ret, seen);
    }
    case ir::TypeTag::Struct:
    case ir::TypeTag::Enum: return false;
    default: return true;
  }
}

// `&mut T` coerces to `&T`, and `*mut T` to `*T`: one is the other
// with the unique half dropped, which is a shared reborrow of the
// same referent or the same address without the write permission.
bool Checker::coerces_to_shared(ir::TypeIdx expected, ir::TypeIdx actual) {
  const ir::TypeTag etag = tag_of(expected);
  const ir::TypeTag atag = tag_of(actual);
  const bool ref_pair = etag == ir::TypeTag::Ref && atag == ir::TypeTag::MutRef;
  const bool raw_pair =
      etag == ir::TypeTag::RawPtr && atag == ir::TypeTag::RawMutPtr;
  if (!ref_pair && !raw_pair) {
    return false;
  }
  return builder.ref_types()[builder.types()[expected].as_ref()].pointee.idx ==
         builder.ref_types()[builder.types()[actual].as_ref()].pointee.idx;
}

bool Checker::coerces_array_to_slice(ir::TypeIdx expected, ir::TypeIdx actual) {
  const ir::TypeTag etag = tag_of(expected);
  const ir::TypeTag atag = tag_of(actual);
  if (etag != ir::TypeTag::Ref && etag != ir::TypeTag::MutRef) {
    return false;
  }
  if (atag != ir::TypeTag::Ref && atag != ir::TypeTag::MutRef) {
    return false;
  }
  if (etag == ir::TypeTag::MutRef && atag != ir::TypeTag::MutRef) {
    return false;
  }
  const ir::TypeIdx epointee =
      builder.ref_types()[builder.types()[expected].as_ref()].pointee;
  const ir::TypeIdx apointee =
      builder.ref_types()[builder.types()[actual].as_ref()].pointee;
  if (tag_of(epointee) != ir::TypeTag::Slice) {
    return false;
  }
  if (tag_of(apointee) != ir::TypeTag::Array) {
    return false;
  }
  const ir::SliceType& slice =
      builder.slice_types()[builder.types()[epointee].as_slice()];
  const ir::ArrayType& array =
      builder.array_types()[builder.types()[apointee].as_array()];
  return types_equal(slice.element, array.element);
}

// Unifies actual against expected, emitting a mismatch diagnostic.
// The coercions it accepts are the two the language has here, a shared
// reborrow and an array-to-slice view; the result is the actual type.
// Error suppresses follow-on diagnostics.
ir::TypeIdx Checker::unify(ir::TypeIdx expected,
                           ir::TypeIdx actual,
                           diag::Span span,
                           std::string_view what) {
  if (is_error(expected)) {
    return actual;
  }
  if (is_error(actual)) {
    return expected;
  }
  if (types_equal(expected, actual)) {
    return actual;
  }
  if (is_never(expected)) {
    return actual;
  }
  if (is_never(actual)) {
    return expected;
  }
  // `&mut T` where `&T` is expected is a shared reborrow, not a
  // mismatch. The exclusive reference is consumed at the call, so
  // nothing can write through it while the shared one is live.
  if (coerces_to_shared(expected, actual)) {
    return actual;
  }
  if (coerces_array_to_slice(expected, actual)) {
    return actual;
  }
  // An unwritten slot is the one mismatch with an obvious fix, so it
  // gets its own message instead of two type names.
  const ir::TypeIdx uninit_side = uninit_payload(expected).is_valid() ? expected
                                  : uninit_payload(actual).is_valid()
                                      ? actual
                                      : ir::TypeIdx::invalid();
  if (uninit_side.is_valid()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerUninitializedValueUsed>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
        span, pretty_tag(tag_of(uninit_side == expected ? actual : expected)));
    (void)index;
    return error_type();
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerTypeMismatchExpectedFound>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
      span, what, pretty_tag(tag_of(expected)), pretty_tag(tag_of(actual)));
  (void)index;
  return error_type();
}

const Checker::Local* Checker::lookup_local(std::string_view name) const {
  for (usize i = scopes.size(); i-- > 0;) {
    for (const Local& local : scopes[i]) {
      if (local.name == name) {
        return &local;
      }
    }
  }
  return nullptr;
}

usize Checker::scope_of(std::string_view name) const {
  for (usize i = scopes.size(); i-- > 0;) {
    for (const Local& local : scopes[i]) {
      if (local.name == name) {
        return i;
      }
    }
  }
  return scopes.size();
}

// True for a recognized suffix (tag set), false when absent (tag
// untouched). Suffixes mix letters and digits (`i32`, `usize`), so
// matching runs over known spellings from the end instead of scanning
// classes.
bool Checker::classify_suffix(std::string_view spelling,
                              ir::TypeTag& tag,
                              bool& is_float,
                              diag::Span span) {
  using TT = ir::TypeTag;
  // Split the spelling into its digit body and whatever follows. Both
  // the base prefix and the digit set belong to the number, so the
  // suffix starts right after them: `0xFF` has none (its digits are
  // letters), `0xFFi32` is `i32`, and `42i128` is a suffix this
  // compiler has no type for. Scanning for a trailing letter run
  // instead would miss every suffix that carries a digit.
  usize body = 0;
  u32 base = 10;
  if (spelling.size() > 2 && spelling[0] == '0') {
    switch (spelling[1]) {
      case 'x':
      case 'X':
        body = 2;
        base = 16;
        break;
      case 'b':
      case 'B':
        body = 2;
        base = 2;
        break;
      case 'o':
      case 'O':
        body = 2;
        base = 8;
        break;
      default: break;
    }
  }
  const auto in_base = [base](char c) {
    if (c >= '0' && c <= '9') {
      return static_cast<u32>(c - '0') < base;
    }
    if (base == 16) {
      return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    return false;
  };
  usize digit_end = body;
  while (digit_end < spelling.size()) {
    const char c = spelling[digit_end];
    if (c == '_' || in_base(c)) {
      ++digit_end;
      continue;
    }
    // A decimal literal may carry a fraction point and an exponent,
    // and both belong to the number rather than to the suffix. The
    // exponent only counts when digits actually follow its optional
    // sign, so `1e` leaves the `e` to be reported.
    if (base != 10) {
      break;
    }
    if (c == '.') {
      ++digit_end;
      continue;
    }
    if (c == 'e' || c == 'E') {
      usize next = digit_end + 1;
      if (next < spelling.size() &&
          (spelling[next] == '+' || spelling[next] == '-')) {
        ++next;
      }
      if (next >= spelling.size() || spelling[next] < '0' ||
          spelling[next] > '9') {
        break;
      }
      digit_end = next;
      continue;
    }
    break;
  }
  const std::string_view suffix = spelling.substr(digit_end);
  if (suffix.empty()) {
    return false;
  }
  struct Suffix {
    std::string_view text;
    ir::TypeTag tag;
    bool is_float;
  };
  constexpr Suffix SUFFIXES[] = {
      {"isize", TT::I64, false}, {"usize", TT::U64, false},
      {"i8", TT::I8, false},     {"i16", TT::I16, false},
      {"i32", TT::I32, false},   {"i64", TT::I64, false},
      {"u8", TT::U8, false},     {"u16", TT::U16, false},
      {"u32", TT::U32, false},   {"u64", TT::U64, false},
      {"f32", TT::F32, true},    {"f64", TT::F64, true},
  };
  for (const Suffix& known : SUFFIXES) {
    if (suffix != known.text) {
      continue;
    }
    tag = known.tag;
    if (known.text == "isize") {
      tag = width == ir::PointerWidth::W64 ? TT::I64 : TT::I32;
    } else if (known.text == "usize") {
      tag = width == ir::PointerWidth::W64 ? TT::U64 : TT::U32;
    }
    is_float = known.is_float;
    return true;
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerUnsupportedLiteralSuffix>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnsupportedType,
      span, suffix);
  (void)index;
  tag = TT::Error;
  return true;
}

ir::TypeIdx Checker::check_literal(ast::LiteralIdx value,
                                   const ir::TypeIdx* expected) {
  using LK = ast::LiteralKind;
  const ast::Literal& lit = ast.literals[value];
  switch (lit.kind) {
    case LK::Bool: {
      const ir::TypeIdx type = builder.primitive(ir::TypeTag::I1);
      if (expected != nullptr) {
        return unify(*expected, type, lit.span, "boolean literal");
      }
      return type;
    }
    case LK::String: {
      const ir::TypeIdx type = builder.primitive(ir::TypeTag::Str);
      if (expected != nullptr) {
        return unify(*expected, type, lit.span, "string literal");
      }
      return type;
    }
    case LK::Char: {
      const u32 index = bag.emit<i18n::Key::AnalyzerCharacterLiteralDeferred>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::UnsupportedType, lit.span);
      (void)index;
      return error_type();
    }
    case LK::Integer: {
      ir::TypeTag tag = ir::TypeTag::I32;
      bool is_float = false;
      const bool has_suffix =
          classify_suffix(lit.spelling, tag, is_float, lit.span);
      if (tag == ir::TypeTag::Error) {
        return error_type();
      }
      if (is_float) {
        const u32 index = bag.emit<i18n::Key::AnalyzerFloatSuffixOnInteger>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::TypeMismatch, lit.span);
        (void)index;
        return error_type();
      }
      if (!has_suffix && expected != nullptr &&
          is_integer_tag(tag_of(*expected))) {
        return *expected;
      }
      const ir::TypeIdx type = builder.primitive(tag);
      if (expected != nullptr) {
        return unify(*expected, type, lit.span, "integer literal");
      }
      return type;
    }
    case LK::Float: {
      ir::TypeTag tag = ir::TypeTag::F64;
      bool is_float = true;
      const bool has_suffix =
          classify_suffix(lit.spelling, tag, is_float, lit.span);
      if (tag == ir::TypeTag::Error) {
        return error_type();
      }
      if (has_suffix && !is_float_tag(tag)) {
        const u32 index = bag.emit<i18n::Key::AnalyzerIntegerSuffixOnFloat>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::TypeMismatch, lit.span);
        (void)index;
        return error_type();
      }
      if (!has_suffix && expected != nullptr &&
          is_float_tag(tag_of(*expected))) {
        return *expected;
      }
      const ir::TypeIdx type = builder.primitive(tag);
      if (expected != nullptr) {
        return unify(*expected, type, lit.span, "float literal");
      }
      return type;
    }
  }
}

void Checker::validate_cycles(const ir::Storage& storage) {
  for (const NominalEntry& entry : nominals) {
    if (!entry.complete) {
      continue;
    }
    std::vector<ir::TypeIdx> stack;
    if (has_value_cycle(entry.type, stack, storage)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerRecursiveType>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::RecursiveType,
          entry.span, entry.name);
      (void)index;
    }
  }
  for (const GenericInstance& instance : generic_instances) {
    if (!instance.complete) {
      continue;
    }
    std::vector<ir::TypeIdx> stack;
    if (has_value_cycle(instance.type, stack, storage)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerRecursiveType>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::RecursiveType,
          nominals[instance.nominal].span, nominals[instance.nominal].name);
      (void)index;
    }
  }
}

// Value namespace

const CheckedModule::StaticInfo* Checker::lookup_static(
    u32 module,
    std::string_view name) const {
  for (const CheckedModule::StaticInfo& info : modules[module].statics) {
    if (info.name == name) {
      return &info;
    }
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Value || import.name != name) {
      continue;
    }
    for (const CheckedModule::StaticInfo& info :
         modules[import.target_module].statics) {
      if (info.name == import.member) {
        return &info;
      }
    }
  }
  return nullptr;
}

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

const CheckedModule::FnSig* Checker::lookup_function(
    u32 module,
    std::string_view name) const {
  for (const CheckedModule::FnSig& fn : modules[module].functions) {
    // A generic function's registered signatures are per
    // instantiation; the name still resolves through
    // lookup_generic_fn so each call rebinds its parameters. A
    // method's signature shares the table but not the name: a bare
    // call cannot reach it.
    if (fn.name == name && !fn.is_method &&
        fn_generic_params(fn.item).empty()) {
      return &fn;
    }
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Value || import.name != name) {
      continue;
    }
    for (const CheckedModule::FnSig& fn :
         modules[import.target_module].functions) {
      if (fn.name == import.member && !fn.is_method &&
          fn_generic_params(fn.item).empty()) {
        return &fn;
      }
    }
  }
  return nullptr;
}

bool Checker::find_variant_in(u32 module,
                              std::string_view name,
                              std::vector<VariantMatch>& out) {
  if (module >= nominals_of_module.size()) {
    return false;
  }
  // A variant is declared by an enumeration of this module, so the walk the
  // table replaced asked the package what the module could answer.
  for (u32 pos : nominals_of_module[module]) {
    NominalEntry& entry = nominals[pos];
    const ast::ItemNode& node = ast.items[entry.item];
    if (node.kind != ast::ItemKind::Enum) {
      continue;
    }
    for (u32 i = 0; i < static_cast<u32>(
                            node.payload.get<ast::ItemEnum>().variants.size());
         ++i) {
      if (node.payload.get<ast::ItemEnum>().variants[i].name.name == name) {
        out.push_back({&entry, i});
      }
    }
  }
  return !out.empty();
}

// Variant lookup in scope: current module plus type-namespace imports.
// Ambiguity diagnoses; unknown returns false silently for the caller
// to report in context.
bool Checker::find_variant(u32 module,
                           std::string_view name,
                           diag::Span span,
                           VariantMatch& out) {
  std::vector<VariantMatch> matches;
  find_variant_in(module, name, matches);
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Type) {
      continue;
    }
    NominalEntry* target = find_nominal(import.target_module, import.member);
    if (target != nullptr) {
      const ast::ItemNode& target_node = ast.items[target->item];
      if (target_node.kind != ast::ItemKind::Enum) {
        continue;
      }
      for (u32 i = 0;
           i < static_cast<u32>(
                   target_node.payload.get<ast::ItemEnum>().variants.size());
           ++i) {
        if (target_node.payload.get<ast::ItemEnum>().variants[i].name.name ==
            name) {
          matches.push_back({target, i});
        }
      }
    }
  }
  if (matches.empty()) {
    return false;
  }
  if (matches.size() > 1) {
    const u32 index = bag.emit<i18n::Key::AnalyzerAmbiguousVariant>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
        span, name);
    (void)index;
    return false;
  }
  out = matches[0];
  return true;
}

NominalEntry* Checker::find_nominal_in_scope(u32 module,
                                             std::string_view name) {
  if (NominalEntry* entry = find_nominal(module, name)) {
    return entry;
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Type || import.name != name) {
      continue;
    }
    if (NominalEntry* target =
            find_nominal(import.target_module, import.member)) {
      return target;
    }
  }
  return nullptr;
}

// Collects the generic impls of the package by the nominal each targets. The
// walk runs once: a lookup matches an instantiation by visiting the impls that
// name its nominal, in the order the modules and their items were in, which is
// the order a match would have found them.
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
      if (impl.params.empty()) {
        continue;
      }
      const ast::TypeNode& target = ast.types[impl.type];
      if (target.kind != ast::TypeKind::Path) {
        continue;
      }
      u32 target_module = NO_MODULE;
      std::string_view target_name;
      if (!resolve_type_path(m, target.payload.get<ast::TypePath>().path,
                             target_module, target_name)) {
        continue;
      }
      NominalEntry* target_entry = find_nominal(target_module, target_name);
      if (target_entry == nullptr) {
        continue;
      }
      generic_impls_by_nominal_[nominal_index(target_entry)].emplace_back(m,
                                                                          item);
    }
  }
}

const CheckedModule::MethodInfo* Checker::lookup_inherent_method(
    ir::TypeIdx self,
    std::string_view name) {
  const auto own = methods_by_self_.find(self.idx);
  if (own != methods_by_self_.end()) {
    for (const auto& [owner, position] : own->second) {
      const CheckedModule::MethodInfo& method =
          modules[owner].methods[position];
      if (method.spec != NO_SPEC) {
        continue;
      }
      if (method.name == name) {
        return &method;
      }
    }
  }
  // Generic instantiation: match `impl<...> Nominal<...>` blocks.
  if (const GenericInstance* instance = generic_find(self)) {
    const u32 nominal = instance->nominal;
    const std::vector<ir::TypeIdx> args = instance->args;
    // The impls that can match are the ones targeting this nominal. Finding
    // them by walking every module's items and resolving every impl it held
    // was one lookup's cost, and a package looks one up per instantiation.
    if (!generic_impls_indexed_ || indexed_nominals_ != nominals.size()) {
      index_generic_impls();
    }
    if (nominal >= generic_impls_by_nominal_.size()) {
      return nullptr;
    }
    for (const auto& [m, item] : generic_impls_by_nominal_[nominal]) {
      const ast::ItemNode& node = ast.items[item];
      const ast::ItemImpl& impl = node.payload.get<ast::ItemImpl>();
      const std::span<const ast::TypeIdx> target_args =
          ast.types[impl.type].payload.get<ast::TypePath>().args;
      if (target_args.size() != nominal_params(nominals[nominal]).size()) {
        continue;
      }
      // Target arguments must name impl parameters directly.
      std::vector<std::pair<std::string_view, ir::TypeIdx>> scope;
      bool shape_ok = true;
      for (usize i = 0; i < target_args.size() && shape_ok; ++i) {
        const ast::TypeNode& arg = ast.types[target_args[i]];
        if (arg.kind != ast::TypeKind::Path) {
          shape_ok = false;
          break;
        }
        const ast::Path& path =
            ast.paths[arg.payload.get<ast::TypePath>().path];
        if (path.segments.size() != 1 ||
            !arg.payload.get<ast::TypePath>().args.empty()) {
          shape_ok = false;
          break;
        }
        bool found = false;
        for (const auto& param : impl.params) {
          if (param.name == path.segments[0].name) {
            scope.emplace_back(param.name, args[i]);
            found = true;
            break;
          }
        }
        shape_ok = found;
      }
      if (!shape_ok) {
        continue;
      }
      const CheckedModule::MethodInfo* method =
          instantiate_method(m, self, scope, impl, name);
      if (method != nullptr) {
        return method;
      }
    }
  }
  return nullptr;
}

const CheckedModule::MethodInfo* Checker::lookup_method(ir::TypeIdx self,
                                                        std::string_view name,
                                                        u32 module,
                                                        diag::Span span,
                                                        bool spec_only,
                                                        u32 spec_filter) {
  if (!spec_only) {
    if (const CheckedModule::MethodInfo* inherent =
            lookup_inherent_method(self, name)) {
      return inherent;
    }
  }
  // Spec dispatch: `impl S for T` blocks, after inherent methods.
  // Coherence leaves at most one impl per spec overlapping a concrete
  // type, so every in-scope match is a different spec; two providing the
  // same name is an ambiguity, not a choice.
  u32 target_nominal = 0;
  std::vector<ir::TypeIdx> target_args;
  if (const GenericInstance* instance = generic_find(self)) {
    target_nominal = instance->nominal;
    target_args = instance->args;
  } else {
    const auto declaring = nominal_by_type_.find(self.idx);
    if (declaring == nominal_by_type_.end()) {
      return nullptr;
    }
    target_nominal = declaring->second;
  }
  const CheckedModule::MethodInfo* match = nullptr;
  for (const SpecImplEntry& entry : spec_impls) {
    if (entry.target.nominal != target_nominal ||
        entry.target.args.size() != target_args.size()) {
      continue;
    }
    bool generic_record = false;
    std::vector<std::pair<std::string_view, ir::TypeIdx>> scope;
    bool shape_ok = true;
    for (usize i = 0; i < target_args.size() && shape_ok; ++i) {
      if (entry.target.args[i].is_param) {
        generic_record = true;
        scope.emplace_back(entry.target.args[i].param, target_args[i]);
      } else if (!types_equal(entry.target.args[i].type, target_args[i])) {
        shape_ok = false;
      }
    }
    if (!shape_ok) {
      continue;
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
          if (method.spec == entry.spec && method.name == name) {
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
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          span, name);
      (void)index;
      return nullptr;
    }
    if (generic_record) {
      const ast::ItemImpl& impl =
          ast.items[entry.item].payload.get<ast::ItemImpl>();
      std::vector<ir::TypeIdx> record_spec_args;
      for (const SpecTarget::Arg& shape : entry.spec_args) {
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
      candidate = instantiate_spec_method(
          entry.module, entry.spec, record_spec_args, self, scope, impl, name);
      if (candidate == nullptr) {
        return nullptr;
      }
    }
    match = candidate;
  }
  return match;
}

std::string_view Checker::fn_name(ast::ItemIdx item) const {
  const ast::ItemNode& node = ast.items[item];
  if (node.kind == ast::ItemKind::Fn) {
    return node.payload.get<ast::ItemFn>().name.name;
  }
  if (node.kind == ast::ItemKind::Intrinsic) {
    return node.payload.get<ast::ItemIntrinsic>().name.name;
  }
  return "<fn>";
}

bool Checker::fn_is_unsafe(ast::ItemIdx item) const {
  const ast::ItemNode& node = ast.items[item];
  if (node.kind == ast::ItemKind::Fn) {
    return node.payload.get<ast::ItemFn>().is_unsafe;
  }
  if (node.kind == ast::ItemKind::Intrinsic) {
    return node.payload.get<ast::ItemIntrinsic>().is_unsafe;
  }
  return false;
}

std::span<const ast::Ident> Checker::fn_generic_params(
    ast::ItemIdx item) const {
  const ast::ItemNode& node = ast.items[item];
  if (node.kind == ast::ItemKind::Fn) {
    return node.payload.get<ast::ItemFn>().generic;
  }
  if (node.kind == ast::ItemKind::Intrinsic) {
    return node.payload.get<ast::ItemIntrinsic>().generic;
  }
  return {};
}

std::span<const ast::ItemFnParam> Checker::fn_params(ast::ItemIdx item) const {
  const ast::ItemNode& node = ast.items[item];
  if (node.kind == ast::ItemKind::Fn) {
    return node.payload.get<ast::ItemFn>().params;
  }
  if (node.kind == ast::ItemKind::Intrinsic) {
    return node.payload.get<ast::ItemIntrinsic>().params;
  }
  return {};
}

ast::TypeIdx Checker::fn_return_type(ast::ItemIdx item) const {
  const ast::ItemNode& node = ast.items[item];
  if (node.kind == ast::ItemKind::Fn) {
    return node.payload.get<ast::ItemFn>().return_type;
  }
  if (node.kind == ast::ItemKind::Intrinsic) {
    return node.payload.get<ast::ItemIntrinsic>().return_type;
  }
  return ast::TypeIdx::invalid();
}

// Index of a type parameter in a declaration's parameter list, or the
// parameter count when the name is not a parameter.
static u32 param_slot(std::span<const ast::Ident> params,
                      std::string_view name) {
  for (u32 i = 0; i < static_cast<u32>(params.size()); ++i) {
    if (params[i].name == name) {
      return i;
    }
  }
  return static_cast<u32>(params.size());
}

Checker::DeclaredBinding Checker::declared_binding(
    std::span<const ast::Ident> params,
    const ast::TypeNode& declared) const {
  const DeclaredBinding none{static_cast<u32>(params.size()), false, false,
                             false};
  if (declared.kind == ast::TypeKind::Ref) {
    const DeclaredBinding inner = declared_binding(
        params, ast.types[declared.payload.get<ast::TypeRef>().inner]);
    return DeclaredBinding{inner.slot, true, inner.through_uninit,
                           inner.through_slice};
  }
  if (declared.kind == ast::TypeKind::RawPtr) {
    const DeclaredBinding inner = declared_binding(
        params, ast.types[declared.payload.get<ast::TypeRawPtr>().inner]);
    return DeclaredBinding{inner.slot, false, inner.through_uninit,
                           inner.through_slice, true};
  }
  if (declared.kind == ast::TypeKind::Slice) {
    // `&[T]` pins `T` from the array or slice behind the reference.
    const DeclaredBinding inner = declared_binding(
        params, ast.types[declared.payload.get<ast::TypeSlice>().element]);
    return DeclaredBinding{inner.slot, inner.through_ref, inner.through_uninit,
                           true};
  }
  if (declared.kind != ast::TypeKind::Path) {
    return none;
  }
  const ast::TypePath& type_path = declared.payload.get<ast::TypePath>();
  const ast::Path& path = ast.paths[type_path.path];
  if (path.segments.size() == 1 && path.segments[0].name == "MaybeUninit" &&
      type_path.args.size() == 1) {
    // `MaybeUninit<T>` pins `T` from the wrapper's payload.
    const DeclaredBinding inner =
        declared_binding(params, ast.types[type_path.args[0]]);
    return DeclaredBinding{inner.slot, inner.through_ref, true,
                           inner.through_slice};
  }
  if (!type_path.args.empty() || path.segments.size() != 1) {
    return none;
  }
  return DeclaredBinding{param_slot(params, path.segments[0].name), false,
                         false, false};
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
  const std::span<const ast::Ident> params = fn_generic_params(item);
  const std::vector<std::pair<std::string_view, ir::TypeIdx>> kept_outer =
      type_params;
  type_params.clear();
  for (usize i = 0; i < params.size() && i < args.size(); ++i) {
    type_params.emplace_back(params[i].name, args[i]);
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
  if (is_intrinsic &&
      !check_intrinsic_signature(
          module, ast.items[item].payload.get<ast::ItemIntrinsic>(), sig_params,
          ret)) {
    type_params = kept_outer;
    return nullptr;
  }
  add_function(module, {.name = fn_name(item),
                        .params = sig_params,
                        .ret = ret,
                        .item = item,
                        .inst = NO_INST,
                        .is_unsafe = fn_is_unsafe(item)});
  const u32 sig_index = static_cast<u32>(modules[module].functions.size()) - 1;
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
  type_params = kept_outer;
  return &modules[module].functions[sig_index];
}

// Binds a generic function or intrinsic's type parameters, then
// instantiates. Explicit turbofish arguments win; otherwise a parameter
// a declared parameter type pins on its own binds from that argument.
const CheckedModule::FnSig* Checker::resolve_generic_fn(
    u32 module,
    ast::ItemIdx item,
    const std::span<const ast::ExprIdx>& args,
    const std::span<const ast::TypeIdx>& explicit_args,
    diag::Span span) {
  const std::span<const ast::Ident> params = fn_generic_params(item);
  if (explicit_args.size() != params.size() && !explicit_args.empty()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTypeArityMismatch>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityMismatch,
        span, fn_name(item), params.size(), params.size() == 1 ? "" : "s");
    (void)index;
    return nullptr;
  }
  std::vector<ir::TypeIdx> bound(params.size(), ir::TypeIdx(base::INVALID_IDX));
  for (usize i = 0; i < explicit_args.size(); ++i) {
    bound[i] = resolve_type(module, explicit_args[i], nullptr);
  }
  for (usize i = 0; i < fn_params(item).size() && i < args.size(); ++i) {
    const DeclaredBinding declared =
        declared_binding(params, ast.types[fn_params(item)[i].type]);
    if (declared.slot >= params.size() || bound[declared.slot].is_valid()) {
      continue;
    }
    const ir::TypeIdx actual = check_expr(module, args[i], nullptr);
    if (is_error(actual)) {
      return nullptr;
    }
    if (!declared.through_ref && !declared.through_raw) {
      // A field's type is a storage copy; the parameter binds the
      // declared type behind it rather than the copy.
      bound[declared.slot] = type_origin(actual);
      continue;
    }
    const ir::TypeTag tag = builder.types()[actual.idx].tag;
    const bool pointer_ok = declared.through_raw ? ir::is_raw_ptr_type(tag)
                                                 : (tag == ir::TypeTag::Ref ||
                                                    tag == ir::TypeTag::MutRef);
    if (!pointer_ok) {
      const u32 index =
          declared.through_raw
              ? bag.emit<i18n::Key::AnalyzerTypeArgumentNeedsPointer>(
                    diag::Severity::Error, diag::Stage::Analyzer,
                    DiagCode::InvalidOperation, span, fn_name(item))
              : bag.emit<i18n::Key::AnalyzerTypeArgumentNeedsReference>(
                    diag::Severity::Error, diag::Stage::Analyzer,
                    DiagCode::InvalidOperation, span, fn_name(item));
      (void)index;
      return nullptr;
    }
    ir::TypeIdx bound_type =
        builder.ref_types()[builder.types()[actual.idx].as_ref()].pointee;
    if (declared.through_uninit) {
      bound_type = uninit_payload(bound_type);
      if (!bound_type.is_valid()) {
        const u32 index = bag.emit<i18n::Key::AnalyzerTypeArgumentNeedsUninit>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::InvalidOperation, span, fn_name(item));
        (void)index;
        return nullptr;
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
        const u32 index =
            bag.emit<i18n::Key::AnalyzerTypeArgumentNeedsSequence>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::InvalidOperation, span, fn_name(item));
        (void)index;
        return nullptr;
      }
    }
    // A wrapper's payload and a slice's element are storage copies;
    // the parameter binds the declared type behind them.
    bound[declared.slot] = type_origin(bound_type);
  }
  for (const ir::TypeIdx arg : bound) {
    if (!arg.is_valid()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerCannotInferTypeArguments>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, span, fn_name(item), fn_name(item));
      (void)index;
      return nullptr;
    }
  }
  // Calling through a type parameter needs a callable bound, which
  // waits on specs: a function type bound to one has no signature
  // its instantiations could mangle distinctly.
  for (const ir::TypeIdx arg : bound) {
    if (!is_error(arg) && builder.types()[arg.idx].tag == ir::TypeTag::Func) {
      const u32 index =
          bag.emit<i18n::Key::AnalyzerFunctionTypeAsGenericArgument>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::UnsupportedType, span);
      (void)index;
      return nullptr;
    }
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
    const ast::ItemImpl& impl,
    std::string_view name) {
  for (ast::ItemIdx method_item : impl.methods) {
    const ast::ItemNode& method_node = ast.items[method_item];
    if (method_node.payload.get<ast::ItemFn>().name.name != name) {
      continue;
    }
    for (const CheckedModule::MethodInfo& existing :
         modules[impl_module].methods) {
      if (existing.item == method_item &&
          existing.self_type.idx == self_type.idx) {
        return &existing;
      }
    }
    // The callee resolves its own parameters only: the caller scope
    // is hidden so a same-named parameter cannot leak through.
    std::vector<std::pair<std::string_view, ir::TypeIdx>> outer_scope =
        std::move(type_params);
    type_params.clear();
    for (const auto& binding : scope) {
      type_params.push_back(binding);
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
    add_method(impl_module,
               {self_type, method_node.payload.get<ast::ItemFn>().name.name,
                params, ret, receiver, method_item,
                check_drop_signature(impl_module, self_type,
                                     modules[impl_module].functions.back(),
                                     receiver, method_item)});
    // Nested instantiations append during the body check; keep the
    // entry this call owns.
    CheckedModule::MethodInfo* entry = &modules[impl_module].methods.back();
    const u32 inst = inst_index(self_type);
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
    type_params = std::move(outer_scope);
    return entry;
  }
  return nullptr;
}

// Synthesizes one spec method entry for a generic instantiation and
// checks its body under the substitution. The declared signature is
// the check: the impl method resolves beside it, and any mismatch is
// reported where the impl is written rather than at the call.
const CheckedModule::MethodInfo* Checker::instantiate_spec_method(
    u32 impl_module,
    u32 spec,
    std::span<const ir::TypeIdx> spec_args,
    ir::TypeIdx self_type,
    const std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
    const ast::ItemImpl& impl,
    std::string_view name) {
  for (ast::ItemIdx method_item : impl.methods) {
    const ast::ItemNode& method_node = ast.items[method_item];
    if (method_node.payload.get<ast::ItemFn>().name.name != name) {
      continue;
    }
    for (const CheckedModule::MethodInfo& existing :
         modules[impl_module].methods) {
      if (existing.item == method_item &&
          existing.self_type.idx == self_type.idx) {
        return &existing;
      }
    }
    std::vector<ir::TypeIdx> declared_params;
    ir::TypeIdx declared_ret = error_type();
    CheckedModule::ReceiverKind declared_receiver =
        CheckedModule::ReceiverKind::None;
    // The callee resolves its own parameters only: the caller scope
    // is hidden so a same-named parameter cannot leak through.
    std::vector<std::pair<std::string_view, ir::TypeIdx>> outer_scope =
        std::move(type_params);
    type_params.clear();
    for (const auto& binding : scope) {
      type_params.push_back(binding);
    }
    if (!spec_method_sig(spec, name, spec_args, self_type, impl_module,
                         declared_params, declared_ret, declared_receiver)) {
      type_params = std::move(outer_scope);
      return nullptr;
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
    bool matches = params.size() == declared_params.size() &&
                   receiver == declared_receiver &&
                   types_equal(ret, declared_ret);
    for (usize i = 0; matches && i < params.size(); ++i) {
      matches = types_equal(params[i], declared_params[i]);
    }
    if (!matches) {
      const SpecEntry& entry = specs[spec];
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecSignatureMismatch>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
          method_node.span, name, entry.name);
      (void)index;
      type_params = std::move(outer_scope);
      return nullptr;
    }
    add_function(impl_module, {method_node.payload.get<ast::ItemFn>().name.name,
                               params, ret, method_item})
        .is_method = true;
    CheckedModule::MethodInfo* entry = &add_method(
        impl_module,
        {self_type, method_node.payload.get<ast::ItemFn>().name.name, params,
         ret, receiver, method_item, false, spec});
    const u32 inst = inst_index(self_type);
    spec_scope.push_back(spec);
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
    spec_scope.pop_back();
    type_params = std::move(outer_scope);
    return entry;
  }
  return nullptr;
}

void Checker::check_spec_impl_bodies(u32 module, ast::ItemIdx item) {
  const ast::ItemImpl& impl = ast.items[item].payload.get<ast::ItemImpl>();
  for (const SpecImplEntry& entry : spec_impls) {
    if (entry.item != item) {
      continue;
    }
    std::vector<ir::TypeIdx> args;
    for (const SpecTarget::Arg& arg : entry.target.args) {
      if (arg.is_param) {
        return;
      }
      args.push_back(arg.type);
    }
    ir::TypeIdx self_type = error_type();
    if (args.empty()) {
      self_type = intern_nominal(nominals[entry.target.nominal]);
    } else {
      self_type = instantiate_generic(entry.target.nominal, args, entry.span);
    }
    if (is_error(self_type)) {
      return;
    }
    spec_scope.push_back(entry.spec);
    for (ast::ItemIdx method_item : impl.methods) {
      check_fn(module, method_item, &self_type);
    }
    spec_scope.pop_back();
    return;
  }
}

void Checker::record_call(u32 module,
                          ast::ExprIdx callee,
                          const CheckedModule::FnSig* fn) {
  const auto found = position_by_address_.find(fn);
  if (found == position_by_address_.end()) {
    return;
  }
  CheckedModule& target = modules[module];
  const u32 index = static_cast<u32>(target.call_targets.size());
  target.call_targets.push_back(
      {callee, false, found->second.first, found->second.second, cur_inst});
  target.call_target_by_key.emplace(
      CheckedModule::call_target_key(callee, cur_inst), index);
}

void Checker::record_call(u32 module,
                          ast::ExprIdx callee,
                          const CheckedModule::MethodInfo* method) {
  const auto found = position_by_address_.find(method);
  if (found == position_by_address_.end()) {
    return;
  }
  CheckedModule& target = modules[module];
  const u32 index = static_cast<u32>(target.call_targets.size());
  target.call_targets.push_back(
      {callee, true, found->second.first, found->second.second, cur_inst});
  target.call_target_by_key.emplace(
      CheckedModule::call_target_key(callee, cur_inst), index);
}

CheckedModule::FnSig& Checker::add_function(u32 module,
                                            CheckedModule::FnSig sig) {
  CheckedModule& target = modules[module];
  target.functions.push_back(std::move(sig));
  position_by_address_.emplace(
      &target.functions.back(),
      std::pair<u32, u32>{module,
                          static_cast<u32>(target.functions.size() - 1)});
  return target.functions.back();
}

// Resolves an expression path to its value meaning. Locals shadow
// everything; nominal type names resolve to Kind::Type so callers
// can report "found type" instead of "unknown".
bool Checker::resolve_value_path(u32 module,
                                 ast::PathIdx path,
                                 std::span<const ast::TypeIdx> type_args,
                                 PathValue& out) {
  const ast::Path& node = ast.paths[path];
  if (node.segments.empty()) {
    return false;
  }
  if (node.segments.size() == 1) {
    const std::string_view name = node.segments[0].name;
    if (const Local* local = lookup_local(name)) {
      // A use resolving below the innermost closure boundary was
      // not captured: a closure sees its own bindings and the
      // module scope, nothing else. A use of a capture binding
      // marks its entry used, which is what the unused check reads.
      if (!closure_bounds.empty()) {
        ClosureBound& bound = closure_bounds.back();
        if (scope_of(name) < bound.scope) {
          const u32 index = bag.emit<i18n::Key::AnalyzerNotCaptured>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::InvalidCapture, node.segments[0].span, name);
          (void)index;
        } else if (local->capture != NO_CAPTURE &&
                   local->capture < bound.captures.size()) {
          bound.captures[local->capture].used = true;
        }
      }
      out.kind = PathValue::Kind::Local;
      out.type = local->type;
      return true;
    }
    if (const CheckedModule::StaticInfo* info = lookup_static(module, name)) {
      out.kind = PathValue::Kind::Static;
      out.type = info->type;
      return true;
    }
    if (const CheckedModule::FnSig* fn = lookup_function(module, name)) {
      out.kind = PathValue::Kind::Function;
      out.function = fn;
      return true;
    }
    if (ast::ItemIdx generic = lookup_generic_fn(module, name);
        generic.is_valid()) {
      out.kind = PathValue::Kind::GenericFn;
      out.generic_item = generic;
      return true;
    }
    VariantMatch match;
    if (find_variant(module, name, node.span, match)) {
      const ast::ItemNode& decl = ast.items[match.enom->item];
      out.enom = match.enom;
      out.variant = match.variant;
      out.type = nominal_owner_type(match.enom);
      if (decl.payload.get<ast::ItemEnum>()
              .variants[match.variant]
              .fields.empty()) {
        out.kind = PathValue::Kind::UnitVariant;
      } else {
        out.kind = PathValue::Kind::TupleVariant;
      }
      return true;
    }
    if (find_nominal_in_scope(module, name) != nullptr) {
      out.kind = PathValue::Kind::Type;
      return true;
    }
    emit_unresolved(bag, diag::Stage::Analyzer, DiagCode::UnknownValue,
                    node.span, std_hints, "value", name);
    return false;
  }
  if (node.segments.size() == 2) {
    const std::string_view head = node.segments[0].name;
    const std::string_view member = node.segments[1].name;
    const u32 child = (head == "package" || head == "self" || head == "super")
                          ? NO_MODULE
                          : find_child_module(module, head);
    if (child != NO_MODULE || head == "package" || head == "self" ||
        head == "super") {
      u32 target = NO_MODULE;
      if (!walk_module_prefix(module, path, "value", target)) {
        return false;
      }
      if (const CheckedModule::StaticInfo* info =
              lookup_static(target, member)) {
        out.kind = PathValue::Kind::Static;
        out.type = info->type;
        return true;
      }
      if (const CheckedModule::FnSig* fn = lookup_function(target, member)) {
        out.kind = PathValue::Kind::Function;
        out.function = fn;
        return true;
      }
      std::vector<VariantMatch> matches;
      if (find_variant_in(target, member, matches) && matches.size() == 1) {
        const ast::ItemNode& decl = ast.items[matches[0].enom->item];
        out.enom = matches[0].enom;
        out.variant = matches[0].variant;
        out.type = nominal_owner_type(matches[0].enom);
        out.kind = decl.payload.get<ast::ItemEnum>()
                           .variants[matches[0].variant]
                           .fields.empty()
                       ? PathValue::Kind::UnitVariant
                       : PathValue::Kind::TupleVariant;
        return true;
      }
      emit_unresolved(bag, diag::Stage::Analyzer, DiagCode::UnknownValue,
                      node.span, std_hints, "value", member);
      return false;
    }
    // Nominal prefix: `Enum::Variant` or `Type::assoc`.
    NominalEntry* nominal = find_nominal_in_scope(module, head);
    if (nominal != nullptr) {
      const ast::ItemNode& nominal_node = ast.items[nominal->item];
      if (nominal_node.kind == ast::ItemKind::Enum) {
        for (u32 i = 0;
             i < static_cast<u32>(
                     nominal_node.payload.get<ast::ItemEnum>().variants.size());
             ++i) {
          if (nominal_node.payload.get<ast::ItemEnum>().variants[i].name.name ==
              member) {
            out.enom = nominal;
            out.variant = i;
            out.type = nominal_owner_type(nominal);
            out.kind = nominal_node.payload.get<ast::ItemEnum>()
                               .variants[i]
                               .fields.empty()
                           ? PathValue::Kind::UnitVariant
                           : PathValue::Kind::TupleVariant;
            return true;
          }
        }
      }
    }
    if (nominal != nullptr) {
      const bool generic_owner = !nominal_params(*nominal).empty();
      ir::TypeIdx self = error_type();
      if (generic_owner) {
        // `Name::<T>::member` names the member of one instantiation.
        if (type_args.empty()) {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerAssociatedFunctionNeedsTypeArguments>(
                  diag::Severity::Error, diag::Stage::Analyzer,
                  DiagCode::GenericArguments, node.span, head, head, member);
          (void)index;
          return false;
        }
        std::vector<ir::TypeIdx> args;
        for (ast::TypeIdx arg : type_args) {
          args.push_back(resolve_type(module, arg, nullptr));
        }
        self = instantiate_generic(static_cast<u32>(nominal - nominals.data()),
                                   args, node.span);
        if (is_error(self)) {
          return false;
        }
      } else if (type_args.empty()) {
        self = intern_nominal(*nominal);
      } else {
        const u32 index = bag.emit<i18n::Key::AnalyzerUnexpectedTypeArguments>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::GenericArguments, node.span, head);
        (void)index;
        return false;
      }
      if (const CheckedModule::MethodInfo* method =
              lookup_method(self, member, module, node.span, false)) {
        if (method->receiver == CheckedModule::ReceiverKind::None) {
          out.kind = PathValue::Kind::AssocFunction;
          out.method = method;
          return true;
        }
      }
    }
    const std::string_view package = std_hint_package(std_hints, head);
    if (!package.empty()) {
      const u32 index =
          bag.emit<i18n::Key::AnalyzerUnresolvedValueInStandardLibrary>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::UnknownValue, node.span, head, member, head, package);
      (void)index;
      return false;
    }
    const u32 index = bag.emit<i18n::Key::AnalyzerUnresolvedValue>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
        node.span, head, member);
    (void)index;
    return false;
  }
  u32 target = NO_MODULE;
  if (!walk_module_prefix(module, path, "value", target)) {
    return false;
  }
  const std::string_view member = node.segments.back().name;
  if (const CheckedModule::StaticInfo* info = lookup_static(target, member)) {
    out.kind = PathValue::Kind::Static;
    out.type = info->type;
    return true;
  }
  if (const CheckedModule::FnSig* fn = lookup_function(target, member)) {
    out.kind = PathValue::Kind::Function;
    out.function = fn;
    return true;
  }
  emit_unresolved(bag, diag::Stage::Analyzer, DiagCode::UnknownValue, node.span,
                  std_hints, "value", member);
  return false;
}

// Resolves a pattern/callee path to an enum variant: a bare name
// in scope, `Enum::Variant`, or `module::Variant`. A generic owner's
// type is left unresolved here; the scrutinee or the call
// expectation fixes the instantiation later.
bool Checker::resolve_variant_path(u32 module,
                                   ast::PathIdx path,
                                   PathValue& out) {
  const ast::Path& node = ast.paths[path];
  if (node.segments.empty()) {
    return false;
  }
  if (node.segments.size() == 1) {
    const std::string_view name = node.segments[0].name;
    VariantMatch match;
    if (find_variant(module, name, node.span, match)) {
      out.kind = PathValue::Kind::TupleVariant;
      out.enom = match.enom;
      out.variant = match.variant;
      out.type = nominal_owner_type(match.enom);
      return true;
    }
    return false;
  }
  if (node.segments.size() == 2) {
    const std::string_view head = node.segments[0].name;
    const std::string_view member = node.segments[1].name;
    const u32 child = (head == "package" || head == "self" || head == "super")
                          ? NO_MODULE
                          : find_child_module(module, head);
    if (child != NO_MODULE || head == "package" || head == "self" ||
        head == "super") {
      u32 target = NO_MODULE;
      if (!walk_module_prefix(module, path, "value", target)) {
        return false;
      }
      std::vector<VariantMatch> matches;
      if (find_variant_in(target, member, matches) && matches.size() == 1) {
        out.kind = PathValue::Kind::TupleVariant;
        out.enom = matches[0].enom;
        out.variant = matches[0].variant;
        out.type = nominal_owner_type(matches[0].enom);
        return true;
      }
      return false;
    }
    if (NominalEntry* nominal = find_nominal_in_scope(module, head)) {
      const ast::ItemNode& nominal_node = ast.items[nominal->item];
      if (nominal_node.kind == ast::ItemKind::Enum) {
        for (u32 i = 0;
             i < static_cast<u32>(
                     nominal_node.payload.get<ast::ItemEnum>().variants.size());
             ++i) {
          if (nominal_node.payload.get<ast::ItemEnum>().variants[i].name.name ==
              member) {
            out.kind = PathValue::Kind::TupleVariant;
            out.enom = nominal;
            out.variant = i;
            out.type = nominal_owner_type(nominal);
            return true;
          }
        }
      }
    }
    return false;
  }
  return false;
}

// Payload types of a resolved variant against a known enum type.
// Callers push the owner's type-parameter substitution first when
// the owner is a generic instantiation, so `resolve_type` sees `T`.
std::vector<ir::TypeIdx> Checker::variant_payloads(const PathValue& resolved,
                                                   ir::TypeIdx enum_type,
                                                   diag::Span span) {
  if (is_error(enum_type) || resolved.enom == nullptr) {
    return {};
  }
  (void)span;
  const ast::ItemNode& decl = ast.items[resolved.enom->item];
  const std::span<const ast::ItemEnumVariant>& variants =
      decl.payload.get<ast::ItemEnum>().variants;
  std::vector<ir::TypeIdx> payloads;
  payloads.reserve(variants[resolved.variant].fields.size());
  for (ast::TypeIdx field : variants[resolved.variant].fields) {
    payloads.push_back(resolve_type(resolved.enom->module, field, nullptr));
  }
  return payloads;
}

NominalEntry* Checker::resolve_struct_path(u32 module, ast::PathIdx path) {
  const ast::Path& node = ast.paths[path];
  if (node.segments.empty()) {
    return nullptr;
  }
  if (node.segments.size() == 1) {
    NominalEntry* nominal =
        find_nominal_in_scope(module, node.segments[0].name);
    if (nominal != nullptr &&
        ast.items[nominal->item].kind == ast::ItemKind::Struct) {
      return nominal;
    }
    emit_unresolved(bag, diag::Stage::Analyzer, DiagCode::UnknownValue,
                    node.span, std_hints, "struct", node.segments[0].name);
    return nullptr;
  }
  u32 target = NO_MODULE;
  if (!walk_module_prefix(module, path, "value", target)) {
    return nullptr;
  }
  NominalEntry* nominal = find_nominal(target, node.segments.back().name);
  if (nominal != nullptr &&
      ast.items[nominal->item].kind == ast::ItemKind::Struct) {
    return nominal;
  }
  emit_unresolved(bag, diag::Stage::Analyzer, DiagCode::UnknownValue, node.span,
                  std_hints, "struct", node.segments.back().name);
  return nullptr;
}

bool Checker::contains_mut_ref(ir::TypeIdx idx, std::vector<u32>& visited) {
  for (u32 seen : visited) {
    if (seen == idx.idx) {
      return false;
    }
  }
  visited.push_back(idx.idx);
  const ir::TypeNode& node = builder.types()[idx];
  switch (node.tag) {
    case ir::TypeTag::MutRef: return true;
    case ir::TypeTag::Ref:
      return contains_mut_ref(builder.ref_types()[node.as_ref()].pointee,
                              visited);
    case ir::TypeTag::Struct: {
      const ir::StructType& struct_type =
          builder.struct_types()[node.as_struct()];
      for (ir::TypeIdx field : struct_type.fields) {
        if (contains_mut_ref(field, visited)) {
          return true;
        }
      }
      return false;
    }
    case ir::TypeTag::Array:
      return contains_mut_ref(builder.array_types()[node.as_array()].element,
                              visited);
    case ir::TypeTag::Slice:
      return contains_mut_ref(builder.slice_types()[node.as_slice()].element,
                              visited);
    case ir::TypeTag::Enum: {
      const ir::EnumType& enum_type = builder.enum_types()[node.as_enum()];
      for (ir::EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = ir::EnumVariantTypeIdx(vidx.idx + 1)) {
        for (ir::TypeIdx field : builder.enum_variant_types()[vidx].fields) {
          if (contains_mut_ref(field, visited)) {
            return true;
          }
        }
      }
      return false;
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType& tuple = builder.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : tuple.elements) {
        if (contains_mut_ref(element, visited)) {
          return true;
        }
      }
      return false;
    }
    default: return false;
  }
}

// Whether a value of the type copies, in the checker's own walk: the
// table's `ir::is_copy_type` runs on cycle-free storage only, and a
// type under checking is not settled yet. A revisit answers Copy,
// because the cycle itself is the error the checker reports
// elsewhere, and mirroring `is_copy_type` is what keeps the two
// answers from drifting.
bool Checker::capture_is_copy(ir::TypeIdx idx, std::vector<u32>& visited) {
  for (u32 seen : visited) {
    if (seen == idx.idx) {
      return true;
    }
  }
  visited.push_back(idx.idx);
  const ir::TypeNode& node = builder.types()[idx];
  switch (node.tag) {
    case ir::TypeTag::MutRef: return false;
    case ir::TypeTag::Error: return true;
    case ir::TypeTag::Ref:
    case ir::TypeTag::Void:
    case ir::TypeTag::Never:
    case ir::TypeTag::I1:
    case ir::TypeTag::I8:
    case ir::TypeTag::I16:
    case ir::TypeTag::I32:
    case ir::TypeTag::I64:
    case ir::TypeTag::U8:
    case ir::TypeTag::U16:
    case ir::TypeTag::U32:
    case ir::TypeTag::U64:
    case ir::TypeTag::F32:
    case ir::TypeTag::F64:
    case ir::TypeTag::Str:
    case ir::TypeTag::Ptr:
    case ir::TypeTag::RawPtr:
    case ir::TypeTag::RawMutPtr:
    case ir::TypeTag::Function:
    case ir::TypeTag::Func: return true;
    case ir::TypeTag::Struct: {
      const ir::StructType& struct_type =
          builder.struct_types()[node.as_struct()];
      for (ir::TypeIdx field : struct_type.fields) {
        if (!capture_is_copy(field, visited)) {
          return false;
        }
      }
      return true;
    }
    case ir::TypeTag::Array:
      return capture_is_copy(builder.array_types()[node.as_array()].element,
                             visited);
    case ir::TypeTag::Slice:
      return capture_is_copy(builder.slice_types()[node.as_slice()].element,
                             visited);
    case ir::TypeTag::Enum: {
      const ir::EnumType& enum_type = builder.enum_types()[node.as_enum()];
      for (ir::EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = ir::EnumVariantTypeIdx(vidx.idx + 1)) {
        for (ir::TypeIdx field : builder.enum_variant_types()[vidx].fields) {
          if (!capture_is_copy(field, visited)) {
            return false;
          }
        }
      }
      return true;
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType& tuple = builder.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : tuple.elements) {
        if (!capture_is_copy(element, visited)) {
          return false;
        }
      }
      return true;
    }
  }
  return true;
}

void Checker::check_fn(u32 module, ast::ItemIdx fn, const ir::TypeIdx* self) {
  const ast::ItemNode& node = ast.items[fn];
  // One report per function: the budget is per function, and every
  // frame past it would otherwise repeat the same diagnostic.
  reported_too_deep_ = false;
  fn_ret = builder.primitive(ir::TypeTag::Void);
  if (node.payload.get<ast::ItemFn>().return_type.is_valid()) {
    fn_ret =
        resolve_type(module, node.payload.get<ast::ItemFn>().return_type, self);
  }
  in_fn = true;
  scopes.emplace_back();
  loop_depth = 0;
  for (const ast::ItemFnParam& param : node.payload.get<ast::ItemFn>().params) {
    const ir::TypeIdx type = resolve_type(module, param.type, self);
    bind_comp_known = param.is_comp;
    const bool refutable = bind_pattern(module, param.pattern, type);
    bind_comp_known = false;
    if (refutable) {
      const u32 index =
          bag.emit<i18n::Key::AnalyzerRefutablePatternInParameter>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::RefutableLet, ast.patterns[param.pattern].span);
      (void)index;
    }
  }
  if (node.payload.get<ast::ItemFn>().body.is_valid()) {
    check_block(module, node.payload.get<ast::ItemFn>().body, &fn_ret);
  }
  scopes.pop_back();
  in_fn = false;
}

void Checker::check_main(u32 module, ast::ItemIdx fn) {
  const ast::ItemNode& node = ast.items[fn];
  if (!node.payload.get<ast::ItemFn>().params.empty()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerMainTakesParameters>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadReturn,
        node.span);
    (void)index;
  }
  ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
  if (node.payload.get<ast::ItemFn>().return_type.is_valid()) {
    ret = resolve_type(module, node.payload.get<ast::ItemFn>().return_type,
                       nullptr);
  }
  if (is_error(ret)) {
    return;
  }
  const ir::TypeTag tag = tag_of(ret);
  if (tag == ir::TypeTag::Void || tag == ir::TypeTag::I32) {
    return;
  }
  // An enum return is accepted structurally: the first variant must
  // carry exactly one `()` payload, so the entry thunk can map the
  // first discriminant to exit code 0. See
  // docs/adr/0009-result-option-library-enums.md.
  if (tag == ir::TypeTag::Enum) {
    const ir::EnumType& shape =
        builder.enum_types()[builder.types()[ret].as_enum()];
    if (shape.variants.size() == 2) {
      const ir::EnumVariantType& first =
          builder.enum_variant_types()[shape.variants.head()];
      if (first.fields.size() == 1 && is_void(first.fields.head())) {
        return;
      }
    }
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerMainReturnType>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadReturn,
      node.span);
  (void)index;
}

void Checker::check_bodies() {
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    for (ast::ItemIdx item : tree.modules[m]->items) {
      const ast::ItemNode& node = ast.items[item];
      switch (node.kind) {
        case ast::ItemKind::Fn: {
          // Generic bodies are checked per instantiation at their call
          // sites, where the type parameters are bound.
          if (node.payload.get<ast::ItemFn>().generic.empty()) {
            check_fn(m, item, nullptr);
          }
          if (m == tree.root &&
              node.payload.get<ast::ItemFn>().name.name == "main") {
            check_main(m, item);
          }
          break;
        }
        case ast::ItemKind::Impl: {
          if (node.payload.get<ast::ItemImpl>().spec.is_valid()) {
            check_spec_impl_bodies(m, item);
            break;
          }
          // Generic impls instantiate per method call; their bodies
          // wait for instantiation-time checking.
          if (!node.payload.get<ast::ItemImpl>().params.empty()) {
            break;
          }
          ir::TypeIdx self = error_type();
          const ir::TypeIdx* self_ptr = nullptr;
          const ast::TypeNode& self_node =
              ast.types[node.payload.get<ast::ItemImpl>().type];
          if (self_node.kind == ast::TypeKind::Path &&
              self_node.payload.get<ast::TypePath>().args.empty()) {
            u32 target_module = NO_MODULE;
            std::string_view target_name;
            if (resolve_type_path(m,
                                  self_node.payload.get<ast::TypePath>().path,
                                  target_module, target_name)) {
              if (NominalEntry* entry =
                      find_nominal(target_module, target_name)) {
                if (nominal_params(*entry).empty()) {
                  self = intern_nominal(*entry);
                  self_ptr = &self;
                }
              }
            }
          }
          for (ast::ItemIdx method :
               node.payload.get<ast::ItemImpl>().methods) {
            check_fn(m, method, self_ptr);
          }
          break;
        }
        case ast::ItemKind::Spec: break;
        case ast::ItemKind::Static:
        case ast::ItemKind::Const: {
          std::string_view name;
          ast::TypeIdx type = ast::TypeIdx::invalid();
          ast::ExprIdx init = ast::ExprIdx::invalid();
          const bool is_const = node.kind == ast::ItemKind::Const;
          if (is_const) {
            name = node.payload.get<ast::ItemConst>().name.name;
            type = node.payload.get<ast::ItemConst>().type;
            init = node.payload.get<ast::ItemConst>().init;
          } else {
            name = node.payload.get<ast::ItemStatic>().name.name;
            type = node.payload.get<ast::ItemStatic>().type;
            init = node.payload.get<ast::ItemStatic>().init;
          }
          const ir::TypeIdx declared = resolve_type(m, type, nullptr);
          if (is_const && ast.exprs[init].kind != ast::ExprKind::Literal) {
            const u32 index = bag.emit<i18n::Key::AnalyzerConstNeedsLiteral>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::InvalidOperation, ast.exprs[init].span, name);
            (void)index;
          }
          if (!is_const) {
            std::vector<u32> visited;
            if (contains_mut_ref(declared, visited)) {
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerStaticHoldsMutableRef>(
                      diag::Severity::Error, diag::Stage::Analyzer,
                      DiagCode::InvalidOperation, ast.types[type].span, name);
              (void)index;
            }
          }
          in_fn = false;
          scopes.emplace_back();
          const ir::TypeIdx actual = check_expr(m, init, &declared);
          unify(declared, actual, ast.exprs[init].span, "item initializer");
          scopes.pop_back();
          break;
        }
        default: break;
      }
    }
  }
}

CheckedModule Checker::empty_module(u32 module) {
  CheckedModule checked;
  checked.module = module;
  return checked;
}
base::Result<CheckedPackage, diag::Reported> check_package(
    const ModuleTree& tree,
    ir::PointerWidth width,
    ast::AstArena& ast,
    diag::DiagBag& bag,
    str::StringInterner& strings,
    std::span<const StdHint> std_hints,
    debug::Profiler* profiler) {
  // Consumer precondition: the tree shape and every arena index the
  // checker dereferences are validated before any pass runs, so
  // hand-built trees fail with a diagnostic instead of UB.
  if (base::Result<void, ModuleTreeError> verified = verify_module_tree(tree);
      verified.is_err()) {
    const u32 index = bag.emit<i18n::Key::CodegenInvalidModuleTree>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::InvalidModuleTree,
        describe_module_tree_error(std::move(verified).unwrap_err()));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (base::Result<void, ast::VerificationError> verified =
          ast::verify_file(ast);
      verified.is_err()) {
    const u32 index = bag.emit<i18n::Key::ParserInvalidSyntaxTree>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::InvalidModuleTree,
        ast::describe_verification_error(std::move(verified).unwrap_err()));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  Checker checker{tree, width, ast, bag, strings, std_hints, profiler};
  checker.uninit_name_id = checker.intern_name("MaybeUninit");
  if (checker.uninit_name_id == str::INVALID_STRING_POOL_ID) {
    return base::make_err(diag::Reported{});
  }
  // The type passes run module by module: nominals, then imports and
  // signatures, then bodies and drop glue, each in one sweep. A run
  // whose cost concentrates in one phase shows as the sweep that grew,
  // and the profile scopes ride with the checker because no other part
  // of it is timed.
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(checker.profiler, "nominals",
                                             "analyze");
    checker.register_nominals();
  }
  if (checker.name_table_exhausted_) {
    return base::make_err(diag::Reported{});
  }
  checker.parents.assign(tree.modules.size(), NO_MODULE);
  {
    // The tree names a child by address and this names it by position, so the
    // positions are a table: walking the modules for each child made one pass
    // over the tree cost the square of its size. The same pass fills the
    // children by the name a path calls them and the roots by the identity one
    // spells, which is what name resolution reads.
    std::unordered_map<const ModuleNode*, u32> position_of;
    position_of.reserve(tree.modules.size());
    for (u32 i = 0; i < static_cast<u32>(tree.modules.size()); ++i) {
      position_of.emplace(tree.modules[i], i);
    }
    checker.children_by_tail_.resize(tree.modules.size());
    for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
      for (const ModuleNode* child : tree.modules[m]->children) {
        const auto found = position_of.find(child);
        if (found == position_of.end()) {
          continue;
        }
        checker.parents[found->second] = m;
        // A path calls a child by the tail of its path: what the tree spells
        // after the last separator.
        const std::string& path = child->path;
        const usize slash = path.find_last_of(':');
        const std::string_view tail =
            slash == std::string::npos
                ? std::string_view(path)
                : std::string_view(path).substr(slash + 1);
        checker.children_by_tail_[m].emplace(tail, found->second);
      }
    }
    for (u32 i = 0; i < static_cast<u32>(tree.package_roots.size()); ++i) {
      checker.root_by_identity_.emplace(tree.package_roots[i].identity, i);
    }
  }
  checker.modules.reserve(tree.modules.size());
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(checker.profiler, "signatures",
                                             "analyze");
    for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
      checker.modules.push_back(checker.empty_module(m));
      checker.process_module(m);
    }
  }
  if (checker.name_table_exhausted_) {
    return base::make_err(diag::Reported{});
  }
  // ADR-0053: super-specs resolve, form no cycle, and every
  // implementation owes an implementation of its super for the same
  // target.
  checker.check_superspecs();
  // ADR-0053: every name a manifest seals must resolve to a spec its
  // package declares; a name that resolves to nothing would seal
  // nothing, which is what the list exists to prevent.
  for (const PackagePolicy& policy : tree.package_policies) {
    for (std::string_view sealed_name : policy.suite_only) {
      bool declared = false;
      for (const SpecEntry& spec : checker.specs) {
        if (spec.name == sealed_name &&
            checker.package_of(spec.module) == policy.package) {
          declared = true;
          break;
        }
      }
      if (!declared) {
        const u32 index = bag.emit<i18n::Key::AnalyzerSpecSealUnknown>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::SpecSealUnknown, diag::Span{}, sealed_name,
            policy.package);
        (void)index;
      }
    }
  }
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(checker.profiler, "bodies",
                                             "analyze");
    checker.check_bodies();
  }
  if (checker.name_table_exhausted_) {
    return base::make_err(diag::Reported{});
  }
  // Resolving a generic destructor instantiates it, so this has to run
  // while the type builder is still live and before the type table is
  // moved out.
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(checker.profiler, "drops",
                                             "analyze");
    checker.resolve_drops();
  }
  if (checker.name_table_exhausted_) {
    return base::make_err(diag::Reported{});
  }
  base::Result<ir::VerifiedStorage, ir::VerificationError> built =
      std::move(checker.builder).build();
  if (built.is_err()) {
    const ir::VerificationError error = std::move(built).unwrap_err();
    const u32 index =
        bag.emit<i18n::Key::AnalyzerCheckedTypesFailedVerification>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidIr,
            diag::Span{}, ir::format_as(error.kind), error.index);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  ir::VerifiedStorage storage = std::move(built).unwrap();
  checker.validate_cycles(*storage);
  CheckedPackage package{tree,
                         std::move(storage),
                         std::move(checker.modules),
                         std::move(checker.inst_numbering),
                         std::move(checker.fn_instances),
                         std::move(checker.type_origins_),
                         {},
                         {}};
  package.drop_glue = std::move(checker.drop_glue_);
  package.needs_drop = std::move(checker.needs_drop_);
  // Generic instantiations lower as ordinary nominals; publish their
  // field or variant names under the defining module for lowering
  // lookups. Their types publish in instantiation order for keying.
  for (const GenericInstance& instance : checker.generic_instances) {
    if (!instance.complete) {
      continue;
    }
    const NominalEntry& nominal = checker.nominals[instance.nominal];
    const ast::ItemNode& node = ast.items[nominal.item];
    if (node.kind == ast::ItemKind::Struct) {
      std::vector<std::string_view> fields;
      for (const ast::ItemStructField& field :
           node.payload.get<ast::ItemStruct>().fields) {
        fields.push_back(field.name.name);
      }
      package.modules[nominal.module].structs.push_back(
          {instance.type, std::move(fields)});
    } else {
      std::vector<std::string_view> variants;
      for (const ast::ItemEnumVariant& variant :
           node.payload.get<ast::ItemEnum>().variants) {
        variants.push_back(variant.name.name);
      }
      package.modules[nominal.module].enums.push_back(
          {nominal.name, instance.type, std::move(variants)});
    }
  }
  return base::make_ok(std::move(package));
}

}  // namespace analyzer
