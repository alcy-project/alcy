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
#include "analyzer/fmt.h"
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
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

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

bool Checker::is_movable(ir::TypeIdx type) const {
  return ir::is_movable_type(
      builder.state(), type, not_movable_,
      [this](ir::TypeIdx idx) { return type_origin(idx); });
}

bool Checker::check_type_args_movable(std::span<const ir::TypeIdx> args,
                                      diag::Span span) {
  for (const ir::TypeIdx arg : args) {
    if (!arg.is_valid() || is_error(arg) || tag_of(arg) == ir::TypeTag::Const) {
      continue;
    }
    if (!is_movable(arg)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnmovableTypeArgument>(
          DiagCode::UnmovableType, span);
      (void)index;
      return false;
    }
  }
  return true;
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

// Whether `type` spells `Option<inner>` with `inner` a bare path
// (`Option<Ordering>`): the shape the comparison specs return.
static bool is_option_of(const ast::AstArena& ast,
                         ast::TypeIdx type,
                         std::string_view inner) {
  if (!type.is_valid()) {
    return false;
  }
  const ast::TypeNode& node = ast.types[type];
  if (node.kind != ast::TypeKind::Path) {
    return false;
  }
  const ast::TypePath& path = node.payload.get<ast::TypePath>();
  if (ast.paths[path.path].segments.size() != 1 || path.args.size() != 1) {
    return false;
  }
  if (ast.paths[path.path].segments[0].name != "Option") {
    return false;
  }
  return bare_path_name(ast, path.args[0]) == inner;
}

// The argument path at which the bare path `name` appears in `type`,
// or false when it appears somewhere else: `Option<T>` is [0], a bare
// `T` is empty, and any other shape is not inferrable yet.
static bool spec_arg_path(const ast::AstArena& ast,
                          ast::TypeIdx type,
                          std::string_view name,
                          std::vector<u32>& path) {
  if (!type.is_valid() || ast.types[type].kind != ast::TypeKind::Path) {
    return false;
  }
  const ast::TypePath& declared = ast.types[type].payload.get<ast::TypePath>();
  const std::span<const ast::Ident> segments =
      ast.paths[declared.path].segments;
  if (declared.args.empty() && segments.size() == 1 &&
      segments[0].name == name) {
    return true;
  }
  if (declared.args.size() == 1) {
    path.push_back(0);
    if (spec_arg_path(ast, declared.args[0], name, path)) {
      return true;
    }
    path.pop_back();
  }
  return false;
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

// Whether `type` is the primitive `bool`.
static bool is_bool_type(const ast::AstArena& ast, ast::TypeIdx type) {
  if (!type.is_valid()) {
    return false;
  }
  const ast::TypeNode& node = ast.types[type];
  return node.kind == ast::TypeKind::Primitive &&
         node.payload.get<ast::TypePrimitive>().primitive ==
             ast::PrimitiveKind::Bool;
}

// Whether `type` is the `usize` primitive.
static bool is_usize_type(const ast::AstArena& ast, ast::TypeIdx type) {
  if (!type.is_valid()) {
    return false;
  }
  const ast::TypeNode& node = ast.types[type];
  return node.kind == ast::TypeKind::Primitive &&
         node.payload.get<ast::TypePrimitive>().primitive ==
             ast::PrimitiveKind::Usize;
}

// Whether `type` is `&mut [u8]`: the buffer a `Display` implementation
// writes its rendering into.
static bool is_mut_slice_of_u8(const ast::AstArena& ast, ast::TypeIdx type) {
  if (!type.is_valid() || ast.types[type].kind != ast::TypeKind::Ref) {
    return false;
  }
  const ast::TypeRef& ref = ast.types[type].payload.get<ast::TypeRef>();
  if (!ref.is_mut || !ref.inner.is_valid()) {
    return false;
  }
  const ast::TypeNode& inner = ast.types[ref.inner];
  if (inner.kind != ast::TypeKind::Slice) {
    return false;
  }
  const ast::TypeIdx element = inner.payload.get<ast::TypeSlice>().element;
  if (!element.is_valid()) {
    return false;
  }
  const ast::TypeNode& element_node = ast.types[element];
  return element_node.kind == ast::TypeKind::Primitive &&
         element_node.payload.get<ast::TypePrimitive>().primitive ==
             ast::PrimitiveKind::U8;
}

bool Checker::check_operator_spec(const ast::ItemSpec& declaration) {
  // The compiler owns the operators' and the closure capability's
  // specs (ADR-0053, ADR-0116): their declared shape is part of the
  // meaning, so a staged declaration that does not match the canonical
  // shape is an error rather than a different operator. Index and
  // IndexMut take `<I, O>` and one method, PartialEq has one `eq`
  // method over `&Self`, the arithmetic specs one by-value method
  // named after the operator, Eq is a marker over PartialEq that
  // declares nothing, and Fn is the one callable shape.
  enum class Shape : u8 {
    Index,
    IndexMut,
    Equality,
    PartialOrder,
    TotalOrder,
    Formatting,
    Arithmetic,
    Negation,
    Marker,
    Callable,
  };
  struct Canonical {
    std::string_view spec;
    Shape shape;
    // The method name for the shapes whose specs differ only in
    // spelling (`Add`/`add`); empty for the shapes that own theirs.
    std::string_view method;
  };
  static constexpr Canonical CANONICAL[] = {
      {"Index", Shape::Index, {}},
      {"IndexMut", Shape::IndexMut, {}},
      {"PartialEq", Shape::Equality, {}},
      {"PartialOrd", Shape::PartialOrder, {}},
      {"Ord", Shape::TotalOrder, {}},
      {"Display", Shape::Formatting, {}},
      {"Add", Shape::Arithmetic, "add"},
      {"Sub", Shape::Arithmetic, "sub"},
      {"Mul", Shape::Arithmetic, "mul"},
      {"Div", Shape::Arithmetic, "div"},
      {"Rem", Shape::Arithmetic, "rem"},
      {"Neg", Shape::Negation, "neg"},
      {"Eq", Shape::Marker, {}},
      {"Fn", Shape::Callable, {}},
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
        DiagCode::SpecCanonicalShape, declaration.name.span,
        declaration.name.name);
    (void)index;
    return false;
  };
  if (canonical->shape == Shape::Marker) {
    // No parameters and no methods: the super-spec carries the
    // requirement the marker stands for.
    if (!declaration.params.empty() || !declaration.methods.empty()) {
      return wrong();
    }
    return true;
  }
  if (canonical->shape == Shape::Callable) {
    // `spec Fn<P, R> { fn call(self: &Self, args: P) -> R; }`: `P` is
    // the parameters as one tuple and `R` the return slot, so one
    // spec covers every arity (ADR-0116).
    if (declaration.params.size() != 2 || declaration.methods.size() != 1) {
      return wrong();
    }
    const ast::SpecMethod& method = declaration.methods[0];
    if (method.name.name != "call" || method.is_unsafe ||
        !method.generic.empty() || method.params.size() != 2) {
      return wrong();
    }
    if (!is_ref_to(ast, method.params[0].type, false, "Self")) {
      return wrong();
    }
    if (bare_path_name(ast, method.params[1].type) !=
        declaration.params[0].name.name) {
      return wrong();
    }
    if (bare_path_name(ast, method.return_type) !=
        declaration.params[1].name.name) {
      return wrong();
    }
    return true;
  }
  if (canonical->shape == Shape::Equality) {
    // `fn eq(self: &Self, other: &Self) -> bool`, no parameters.
    if (!declaration.params.empty() || declaration.methods.size() != 1) {
      return wrong();
    }
    const ast::SpecMethod& method = declaration.methods[0];
    if (method.name.name != "eq" || method.is_unsafe ||
        !method.generic.empty() || method.params.size() != 2) {
      return wrong();
    }
    if (!is_ref_to(ast, method.params[0].type, false, "Self") ||
        !is_ref_to(ast, method.params[1].type, false, "Self")) {
      return wrong();
    }
    if (!is_bool_type(ast, method.return_type)) {
      return wrong();
    }
    return true;
  }
  if (canonical->shape == Shape::Formatting) {
    // `fn display(self: &Self, out: &mut [u8]) -> usize`, no
    // parameters: the implementation writes the prefix that fits and
    // returns the total length.
    if (!declaration.params.empty() || declaration.methods.size() != 1) {
      return wrong();
    }
    const ast::SpecMethod& method = declaration.methods[0];
    if (method.name.name != "display" || method.is_unsafe ||
        !method.generic.empty() || method.params.size() != 2) {
      return wrong();
    }
    if (!is_ref_to(ast, method.params[0].type, false, "Self") ||
        !is_mut_slice_of_u8(ast, method.params[1].type)) {
      return wrong();
    }
    if (!is_usize_type(ast, method.return_type)) {
      return wrong();
    }
    return true;
  }
  if (canonical->shape == Shape::PartialOrder ||
      canonical->shape == Shape::TotalOrder) {
    // `fn partial_cmp(self: &Self, other: &Self) -> Option<Ordering>`,
    // and `cmp` returning the bare `Ordering` for the total order.
    const bool total = canonical->shape == Shape::TotalOrder;
    if (!declaration.params.empty() || declaration.methods.size() != 1) {
      return wrong();
    }
    const ast::SpecMethod& method = declaration.methods[0];
    if (method.name.name != (total ? "cmp" : "partial_cmp") ||
        method.is_unsafe || !method.generic.empty() ||
        method.params.size() != 2) {
      return wrong();
    }
    if (!is_ref_to(ast, method.params[0].type, false, "Self") ||
        !is_ref_to(ast, method.params[1].type, false, "Self")) {
      return wrong();
    }
    const bool spell =
        total ? bare_path_name(ast, method.return_type) == "Ordering"
              : is_option_of(ast, method.return_type, "Ordering");
    if (!spell) {
      return wrong();
    }
    return true;
  }
  if (canonical->shape == Shape::Arithmetic ||
      canonical->shape == Shape::Negation) {
    // `fn add(self: Self, other: Self) -> Self`, and `Neg` drops the
    // second operand. The receiver goes by value, so an operator
    // consumes its operands.
    const bool unary = canonical->shape == Shape::Negation;
    if (!declaration.params.empty() || declaration.methods.size() != 1) {
      return wrong();
    }
    const ast::SpecMethod& method = declaration.methods[0];
    if (method.name.name != canonical->method || method.is_unsafe ||
        !method.generic.empty() || method.params.size() != (unary ? 1 : 2)) {
      return wrong();
    }
    for (const ast::ItemFnParam& param : method.params) {
      if (bare_path_name(ast, param.type) != "Self") {
        return wrong();
      }
    }
    if (bare_path_name(ast, method.return_type) != "Self") {
      return wrong();
    }
    return true;
  }
  // `<I, O>` with one method taking `&Self` and `I` and returning
  // `&O` (`&mut` on receiver and result for the mutating form).
  const bool mut_receiver = canonical->shape == Shape::IndexMut;
  if (declaration.params.size() != 2 || declaration.methods.size() != 1) {
    return wrong();
  }
  const ast::SpecMethod& method = declaration.methods[0];
  if (method.name.name != (mut_receiver ? "index_mut" : "index") ||
      method.is_unsafe || !method.generic.empty() ||
      method.params.size() != 2) {
    return wrong();
  }
  if (!is_ref_to(ast, method.params[0].type, mut_receiver, "Self")) {
    return wrong();
  }
  if (bare_path_name(ast, method.params[1].type) !=
      declaration.params[0].name.name) {
    return wrong();
  }
  if (!is_ref_to(ast, method.return_type, mut_receiver,
                 declaration.params[1].name.name)) {
    return wrong();
  }
  return true;
}

u32 Checker::operator_spec(std::string_view name) const {
  for (u32 i = 0; i < static_cast<u32>(specs.size()); ++i) {
    if (specs[i].name == name && tree.is_staged_item("core", specs[i].item)) {
      return i;
    }
  }
  return U32_MAX;
}

u32 Checker::fn_spec() const {
  return operator_spec("Fn");
}

u32 Checker::fmt_spec(std::string_view name) const {
  for (u32 i = 0; i < static_cast<u32>(specs.size()); ++i) {
    if (specs[i].name == name && tree.is_staged_item("fmt", specs[i].item)) {
      return i;
    }
  }
  return U32_MAX;
}

bool Checker::has_display(ir::TypeIdx type, u32 module) {
  const u32 spec = fmt_spec("Display");
  if (spec == U32_MAX) {
    return false;
  }
  const CheckedModule::MethodInfo* method =
      lookup_method(type, "display", module, diag::Span{}, true, spec);
  return method != nullptr &&
         method->receiver == CheckedModule::ReceiverKind::Shared &&
         method->params.size() == 2;
}

void Checker::register_spec(u32 module, ast::ItemIdx item) {
  const ast::ItemNode& node = ast.items[item];
  const ast::ItemSpec& spec = node.payload.get<ast::ItemSpec>();
  if (find_nominal(module, spec.name.name) != nullptr) {
    const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
        DiagCode::DuplicateDefinition, spec.name.span, spec.name.name);
    (void)index;
    return;
  }
  if (find_spec(module, spec.name.name) != nullptr) {
    const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
        DiagCode::DuplicateDefinition, spec.name.span, spec.name.name);
    (void)index;
    return;
  }
  for (usize i = 0; i < spec.methods.size(); ++i) {
    for (usize j = 0; j < i; ++j) {
      if (spec.methods[j].name.name == spec.methods[i].name.name) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
            DiagCode::DuplicateDefinition, spec.methods[i].name.span,
            spec.methods[i].name.name);
        (void)index;
        return;
      }
    }
    if (spec.methods[i].is_unsafe) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnsafeMethodUnsupported>(
          DiagCode::InvalidOperation, spec.methods[i].name.span,
          spec.methods[i].name.name);
      (void)index;
      return;
    }
  }
  specs.push_back(SpecEntry{module, spec.name.name, item, node.span});
  specs_of_module[module].push_back(static_cast<u32>(specs.size() - 1));
  // The compiler owns the operator specs (ADR-0053), and `fmt` owns
  // `Display`: a staged declaration of either must keep the canonical
  // shape. It still registers, so a wrong shape is one diagnostic
  // rather than a flood of unresolved names at every use.
  if (tree.is_staged_item("core", item) || tree.is_staged_item("fmt", item)) {
    (void)check_operator_spec(spec);
  }
}

bool Checker::spec_method_sig(u32 spec,
                              std::string_view name,
                              std::span<const ir::TypeIdx> spec_args,
                              ir::TypeIdx self_type,
                              u32 module,
                              std::span<const ir::TypeIdx> method_args,
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
    if (method.generic.size() != method_args.size()) {
      return false;
    }
    const usize pushed = type_params.size();
    for (usize i = 0; i < declaration.params.size(); ++i) {
      type_params.emplace_back(declaration.params[i].name.name, spec_args[i]);
    }
    // A method's own type parameters bind before its signature
    // resolves, the way the spec's do.
    for (usize i = 0; i < method.generic.size(); ++i) {
      type_params.emplace_back(method.generic[i].name.name, method_args[i]);
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

bool Checker::check_spec_method_bounds(u32 spec,
                                       std::string_view name,
                                       std::span<const ir::TypeIdx> spec_args,
                                       std::span<const ir::TypeIdx> method_args,
                                       diag::Span span) {
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
    if (method.generic.size() != method_args.size()) {
      return false;
    }
    std::vector<std::pair<std::string_view, ir::TypeIdx>> bindings;
    bindings.reserve(method.generic.size());
    for (usize i = 0; i < method.generic.size(); ++i) {
      bindings.emplace_back(method.generic[i].name.name, method_args[i]);
    }
    const usize pushed = type_params.size();
    for (usize i = 0; i < declaration.params.size(); ++i) {
      type_params.emplace_back(declaration.params[i].name.name, spec_args[i]);
    }
    const bool ok = check_declared_bounds(specs[spec].module, method.generic,
                                          bindings, span);
    while (type_params.size() > pushed) {
      type_params.pop_back();
    }
    return ok;
  }
  return false;
}

bool Checker::spec_targets_overlap(const SpecTarget& a, const SpecTarget& b) {
  // A scalar target carries no arguments: it overlaps only the same
  // scalar.
  if (a.is_primitive || b.is_primitive) {
    return a.is_primitive && b.is_primitive &&
           types_equal(a.primitive, b.primitive);
  }
  if (a.nominal != b.nominal || a.args.size() != b.args.size()) {
    return false;
  }
  for (usize i = 0; i < a.args.size(); ++i) {
    if (a.args[i].is_param || b.args[i].is_param || a.args[i].is_value ||
        b.args[i].is_value || a.args[i].shape.is_valid() ||
        b.args[i].shape.is_valid()) {
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
  // A scalar target has one shape: the scalar itself.
  if (a.target.is_primitive || b.target.is_primitive) {
    return a.target.is_primitive && b.target.is_primitive &&
           types_equal(a.target.primitive, b.target.primitive);
  }
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
    if (left.is_value != right.is_value) {
      return false;
    }
    if (left.is_value) {
      continue;
    }
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

u32 Checker::bound_spec(u32 module, std::string_view name) {
  if (const SpecEntry* entry = find_spec_in_scope(module, name)) {
    return static_cast<u32>(entry - specs.data());
  }
  for (u32 i = 0; i < static_cast<u32>(specs.size()); ++i) {
    if (specs[i].name == name) {
      return i;
    }
  }
  return U32_MAX;
}

bool Checker::is_callable_with(ir::TypeIdx type,
                               std::span<const ir::TypeIdx> params,
                               ir::TypeIdx ret) {
  // One comparison for both shapes: the witness's parameters and
  // return against the ones the spelling named.
  const auto matches = [&](const auto& have_params, ir::TypeIdx have_ret) {
    if (have_params.size() != params.size()) {
      return false;
    }
    for (usize i = 0; i < params.size(); ++i) {
      if (!types_equal(have_params[i], params[i])) {
        return false;
      }
    }
    return types_equal(have_ret, ret);
  };
  const ir::TypeIdx origin = type_origin(type);
  if (const ClosureSig* sig = closure_sig_of_type(origin)) {
    return matches(sig->params, sig->ret);
  }
  if (tag_of(origin) != ir::TypeTag::Func) {
    return false;
  }
  const ir::FuncType& shape =
      builder.func_types()[builder.types()[origin.idx].as_func()];
  return matches(shape.params, shape.ret);
}

bool Checker::implements_spec(ir::TypeIdx type,
                              u32 spec,
                              std::span<const ir::TypeIdx> spec_args,
                              u32 module) {
  (void)module;
  const ir::TypeIdx origin = type_origin(type);
  // The closure capability has no impl records (ADR-0116): a value is
  // callable by the signature its type carries, a closure nominal's
  // recorded one or a function view's own. `P` is the parameters as
  // one tuple, and a single non-tuple argument reads as the one
  // parameter, so `Fn<i32, R>` and `Fn<(i32,), R>` agree.
  if (spec == fn_spec()) {
    if (spec_args.size() != 2) {
      return false;
    }
    const ir::TypeIdx p = spec_args[0];
    std::vector<ir::TypeIdx> params;
    const ir::TypeTag p_tag = tag_of(p);
    if (p_tag == ir::TypeTag::Tuple) {
      const ir::TupleType tuple =
          builder.tuple_types()[builder.types()[p.idx].as_tuple()];
      params.reserve(tuple.elements.size());
      for (ir::TypeIdx element : tuple.elements) {
        params.push_back(element);
      }
    } else if (p_tag != ir::TypeTag::Void) {
      params.push_back(p);
    }
    return is_callable_with(origin, params, spec_args[1]);
  }
  const ir::TypeTag tag = tag_of(origin);
  bool is_scalar = false;
  u32 nominal = 0;
  std::vector<ir::TypeIdx> args;
  if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Enum) {
    if (const GenericInstance* instance = generic_find(origin)) {
      nominal = instance->nominal;
      args = instance->args;
    } else {
      const auto declaring = nominal_by_type_.find(origin.idx);
      if (declaring == nominal_by_type_.end()) {
        return false;
      }
      nominal = declaring->second;
    }
  } else {
    is_scalar = true;
  }
  for (const SpecImplEntry& candidate : spec_impls) {
    if (candidate.target.is_primitive) {
      if (!is_scalar || !types_equal(candidate.target.primitive, origin)) {
        continue;
      }
    } else {
      if (is_scalar || candidate.target.nominal != nominal ||
          candidate.target.args.size() != args.size()) {
        continue;
      }
      bool match = true;
      for (usize i = 0; i < args.size(); ++i) {
        const SpecTarget::Arg& want = candidate.target.args[i];
        if (want.is_param || want.is_value) {
          continue;
        }
        if (!types_equal(want.type, args[i])) {
          match = false;
          break;
        }
      }
      if (!match) {
        continue;
      }
    }
    if (candidate.spec == spec) {
      if (candidate.spec_args.size() != spec_args.size()) {
        continue;
      }
      bool same = true;
      for (usize i = 0; i < spec_args.size(); ++i) {
        const SpecTarget::Arg& want = candidate.spec_args[i];
        // A parameter or a shape is bound by the receiver match
        // (`impl<T> Iterator<T> for VecIter<T>` matches `Iterator<i32>`
        // once the receiver is `VecIter<i32>`).
        if (want.is_param || want.is_value || want.shape.is_valid()) {
          continue;
        }
        if (!types_equal(want.type, spec_args[i])) {
          same = false;
          break;
        }
      }
      if (same) {
        return true;
      }
      continue;
    }
    // A refined spec carries the ones it refines: `impl Eq for T`
    // satisfies a `PartialEq` bound.
    for (u32 node = candidate.spec; node < spec_supers_.size();) {
      node = spec_supers_[node];
      if (node == U32_MAX) {
        break;
      }
      if (node == spec) {
        return true;
      }
    }
  }
  return false;
}

// Whether `declared` is `required` or refines it through the super
// chain (`Eq` carries `PartialEq`, ADR-0028): an implementation of a
// refined spec implements the ones it refines, so a bound on one
// provides the others' methods.

bool Checker::spec_refines(u32 declared, u32 required) const {
  for (u32 node = declared; node < spec_supers_.size();) {
    node = spec_supers_[node];
    if (node == U32_MAX) {
      return false;
    }
    if (node == required) {
      return true;
    }
  }
  return false;
}

void Checker::declared_bounds(u32 module,
                              const ast::GenericParam& param,
                              std::vector<DeclaredBound>& out) {
  std::vector<ast::TypeIdx> annotations;
  if (param.type.is_valid()) {
    annotations.push_back(param.type);
  }
  for (ast::TypeIdx extra : param.bounds) {
    annotations.push_back(extra);
  }
  for (ast::TypeIdx annotation : annotations) {
    if (ast.types[annotation].kind != ast::TypeKind::Path) {
      continue;
    }
    const ast::TypePath& path =
        ast.types[annotation].payload.get<ast::TypePath>();
    const std::span<const ast::Ident> segments = ast.paths[path.path].segments;
    if (segments.empty()) {
      continue;
    }
    const u32 spec = bound_spec(module, segments.back().name);
    if (spec == U32_MAX) {
      continue;
    }
    out.push_back(DeclaredBound{spec, path});
  }
}

// Whether the declared bounds of the parameter at `index` include
// `spec` over `spec_args`: the requirement a parametric call checks,
// the way an implementation satisfies a concrete instantiation's
// (ADR-0080). The comparison is exact - the spec in scope and the
// arguments pairwise equal - which is the shape the sources write.

bool Checker::param_declares_bound(u32 index,
                                   u32 spec,
                                   std::span<const ir::TypeIdx> spec_args,
                                   u32 module) {
  const ast::GenericParam* declared = param_at(index);
  if (declared == nullptr) {
    return false;
  }
  std::vector<DeclaredBound> bounds;
  declared_bounds(module, *declared, bounds);
  for (const DeclaredBound& bound : bounds) {
    if (bound.spec != spec && !spec_refines(bound.spec, spec)) {
      continue;
    }
    if (bound.path.args.size() != spec_args.size()) {
      continue;
    }
    bool same = true;
    for (usize i = 0; same && i < bound.path.args.size(); ++i) {
      const ir::TypeIdx resolved =
          resolve_type(module, bound.path.args[i], nullptr);
      same = !is_error(resolved) && types_equal(resolved, spec_args[i]);
    }
    if (same) {
      return true;
    }
  }
  return false;
}

ir::TypeIdx Checker::arg_at_path(ir::TypeIdx type,
                                 const std::vector<u32>& path) const {
  ir::TypeIdx at = type;
  for (u32 index : path) {
    const GenericInstance* instance = generic_find(at);
    if (instance == nullptr || index >= instance->args.size()) {
      return ir::TypeIdx::invalid();
    }
    at = instance->args[index];
  }
  return at;
}

bool Checker::infer_bound_arguments(u32 module,
                                    std::span<const ast::GenericParam> params,
                                    std::vector<ir::TypeIdx>& bound,
                                    diag::Span span) {
  for (usize j = 0; j < params.size(); ++j) {
    if (is_value_param(ast, params[j]) || bound[j].is_valid()) {
      continue;
    }
    // A bound on a bound already known names this parameter as one of
    // its spec's arguments (`I: Iterator<T>`): the implementation for
    // `I` answers it, read through the spec method's signature.
    for (usize i = 0; i < params.size() && !bound[j].is_valid(); ++i) {
      if (i == j || is_value_param(ast, params[i]) || !bound[i].is_valid()) {
        continue;
      }
      std::vector<ast::TypeIdx> annotations;
      if (params[i].type.is_valid()) {
        annotations.push_back(params[i].type);
      }
      for (ast::TypeIdx extra : params[i].bounds) {
        annotations.push_back(extra);
      }
      for (ast::TypeIdx annotation : annotations) {
        if (!annotation.is_valid() ||
            ast.types[annotation].kind != ast::TypeKind::Path) {
          continue;
        }
        const ast::TypePath& path =
            ast.types[annotation].payload.get<ast::TypePath>();
        const std::span<const ast::Ident> segments =
            ast.paths[path.path].segments;
        if (segments.empty()) {
          continue;
        }
        const u32 spec = bound_spec(module, segments.back().name);
        if (spec == U32_MAX) {
          continue;
        }
        const ast::ItemSpec& declaration =
            ast.items[specs[spec].item].payload.get<ast::ItemSpec>();
        if (declaration.methods.empty()) {
          continue;
        }
        // The parameter has to be one of the bound's arguments, named
        // on its own (`Iterator<T>`, not `Iterator<Option<T>>`).
        u32 position = 0;
        bool at_argument = false;
        for (usize k = 0; k < path.args.size(); ++k) {
          const ast::TypeIdx arg = path.args[k];
          if (!arg.is_valid() || ast.types[arg].kind != ast::TypeKind::Path) {
            continue;
          }
          const ast::TypePath& arg_path =
              ast.types[arg].payload.get<ast::TypePath>();
          const std::span<const ast::Ident> arg_segments =
              ast.paths[arg_path.path].segments;
          if (arg_path.args.empty() && arg_segments.size() == 1 &&
              arg_segments[0].name == params[j].name.name) {
            position = static_cast<u32>(k);
            at_argument = true;
            break;
          }
        }
        if (!at_argument) {
          continue;
        }
        const CheckedModule::MethodInfo* method =
            lookup_method(bound[i], declaration.methods[0].name.name, module,
                          span, true, spec);
        if (method == nullptr || is_error(method->ret)) {
          continue;
        }
        std::vector<u32> arg_path;
        if (!spec_arg_path(ast, declaration.methods[0].return_type,
                           declaration.params[position].name.name, arg_path)) {
          continue;
        }
        const ir::TypeIdx inferred = arg_at_path(method->ret, arg_path);
        if (!inferred.is_valid() || is_error(inferred)) {
          continue;
        }
        bound[j] = inferred;
      }
    }
  }
  return true;
}

bool Checker::check_declared_bounds(
    u32 module,
    std::span<const ast::GenericParam> params,
    const std::vector<std::pair<std::string_view, ir::TypeIdx>>& bindings,
    diag::Span span) {
  for (const ast::GenericParam& param : params) {
    if (is_value_param(ast, param)) {
      continue;
    }
    const ir::TypeIdx* bound_type = nullptr;
    for (const auto& binding : bindings) {
      if (binding.first == param.name.name) {
        bound_type = &binding.second;
        break;
      }
    }
    if (bound_type == nullptr || !bound_type->is_valid()) {
      continue;
    }
    // A return-position owning-function slot carries its witness from
    // the body (ADR-0075): its bound is checked when the body names
    // one, not against the stand-in the signature resolved with.
    const OwnFnSlot* own = own_fn_slot(param.name.name);
    if (own != nullptr && own->is_return) {
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
      if (ast.types[annotation].kind != ast::TypeKind::Path) {
        // The declaration check owns a spelling that is no spec.
        continue;
      }
      const ast::TypePath& path =
          ast.types[annotation].payload.get<ast::TypePath>();
      const std::span<const ast::Ident> segments =
          ast.paths[path.path].segments;
      if (segments.empty()) {
        continue;
      }
      // The sugar's bound names `core`'s spec by identity, the way
      // operator dispatch does (ADR-0053, ADR-0116): a module-local
      // spec that happens to be called `Fn` is a different capability.
      const u32 spec =
          own != nullptr ? fn_spec() : bound_spec(module, segments.back().name);
      if (spec == U32_MAX) {
        continue;
      }
      // The bound's own arguments resolve under the instantiation's
      // bindings (`Iterator<T>`), the way a spec argument's shape does.
      std::vector<ir::TypeIdx> spec_args;
      for (ast::TypeIdx arg : path.args) {
        const usize pushed = type_params.size();
        for (const auto& binding : bindings) {
          type_params.emplace_back(binding.first, binding.second);
        }
        const ir::TypeIdx resolved = resolve_type(module, arg, nullptr);
        while (type_params.size() > pushed) {
          type_params.pop_back();
        }
        if (is_error(resolved)) {
          bound_failure_ = true;
          return false;
        }
        spec_args.push_back(resolved);
      }
      // `{}` formats a builtin convertible with no impl at all, so a
      // `Display` bound accepts those types too.
      const bool builtin_display =
          analyzer::is_formattable_tag(tag_of(*bound_type)) &&
          specs[spec].name == "Display" &&
          tree.is_staged_item("fmt", specs[spec].item);
      // A scalar carries its operators builtin (ADR-0053), so an
      // arithmetic bound it already satisfies, the way a formattable
      // scalar satisfies `Display`. The spec is the compiler-owned one
      // by identity, not a user spec that shares the name.
      const ir::TypeTag bound_tag = tag_of(*bound_type);
      const bool numeric =
          bound_tag != ir::TypeTag::I1 &&
          (is_integer_tag(bound_tag) || is_float_tag(bound_tag));
      const bool builtin_arithmetic =
          numeric &&
          (spec == operator_spec("Add") || spec == operator_spec("Sub") ||
           spec == operator_spec("Mul") || spec == operator_spec("Div") ||
           spec == operator_spec("Neg") ||
           (is_integer_tag(bound_tag) && spec == operator_spec("Rem")));
      // A parameter the instantiation has not fixed yet meets the
      // definition-site check: what its declared bounds already state
      // satisfies the requirement, the way an implementation would
      // (ADR-0080).
      const bool implied =
          tag_of(*bound_type) == ir::TypeTag::Param &&
          param_declares_bound(
              builder.types()[bound_type->idx].as_param().value, spec,
              spec_args, module);
      if (!builtin_display && !builtin_arithmetic && !implied &&
          !implements_spec(*bound_type, spec, spec_args, module)) {
        // An owning-function slot reports its own spelling (ADR-0075):
        // the witness does not carry the `fn(...)` signature that was
        // written, and the message names the slot rather than the spec.
        if (own != nullptr) {
          const u32 index = bag.emit<i18n::Key::AnalyzerOwnFunctionWitness>(
              DiagCode::TypeMismatch, span, own->display);
          (void)index;
        } else {
          const ast::GenericParam* declared =
              tag_of(*bound_type) == ir::TypeTag::Param
                  ? param_at(builder.types()[bound_type->idx].as_param().value)
                  : nullptr;
          const std::string_view name = declared != nullptr
                                            ? declared->name.name
                                            : type_name(*bound_type);
          const u32 index = bag.emit<i18n::Key::AnalyzerTypeBoundNotSatisfied>(
              DiagCode::InvalidOperation, span, name, specs[spec].name);
          (void)index;
        }
        bound_failure_ = true;
        return false;
      }
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
            if (target != nullptr &&
                !check_item_access(entry.module, target->module,
                                   ast.items[target->item].is_pub,
                                   super_node.span, super_name)) {
              continue;
            }
          }
        }
      }
    }
    if (target == nullptr) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecSuperUnknown>(
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
          DiagCode::SpecSuperCycle, specs[node].span, specs[node].name);
      (void)index;
    }
    for (u32 on_path : path) {
      state[on_path] = 2;
    }
  }
  spec_supers_ = super_of;
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
          DiagCode::SpecSuperMissing, entry.span, specs[entry.spec].name,
          specs[super].name);
      (void)index;
    }
  }
}

// A declaration recognized by its reserved name rather than by path.
// Range and Bound are reserved, so a tree holds at most one of each.

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
          if (spec_entry != nullptr &&
              !check_item_access(module, spec_entry->module,
                                 ast.items[spec_entry->item].is_pub,
                                 spec_node.span, spec_name)) {
            return;
          }
        }
      }
    }
  }
  if (spec_entry == nullptr) {
    const u32 index = bag.emit<i18n::Key::AnalyzerUnresolved>(
        DiagCode::UnknownType, spec_node.span, "spec", spec_spelling);
    (void)index;
    return;
  }
  const u32 spec_index = static_cast<u32>(spec_entry - specs.data());
  // ADR-0053: a spec its package seals to a suite may be implemented
  // only by that package or by a package of that suite. The manifest
  // name list is the seal; the reserved implementing-side key that
  // would open it is not accepted yet. A sealed spec is also the only
  // one a scalar target may name.
  bool sealed = false;
  if (const PackagePolicy* const declaring =
          policy_of(package_of(spec_entry->module));
      declaring != nullptr) {
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
      // The implementing side of the seal: a package outside it may
      // list the spec in its own `[spec] implement`.
      bool listed = false;
      if (implementing != nullptr) {
        for (std::string_view allowed : implementing->implement) {
          if (allowed == spec_entry->name) {
            listed = true;
            break;
          }
        }
      }
      if (!same_package && !same_suite && !listed) {
        const u32 index = bag.emit<i18n::Key::AnalyzerSpecSealed>(
            DiagCode::SpecSealed, spec_node.span, spec_entry->name,
            declaring->package);
        (void)index;
        return;
      }
    }
  }
  const ast::ItemSpec& declaration =
      ast.items[spec_entry->item].payload.get<ast::ItemSpec>();
  const ast::TypeNode& target_node = ast.types[impl.type];
  // A scalar target carries no parameters, so an impl naming any is the
  // concrete implementation it already is.
  const bool generic_impl =
      !impl.params.empty() && target_node.kind != ast::TypeKind::Primitive;
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
          DiagCode::ArityMismatch, spec_node.span, spec_entry->name,
          declaration.params.size(), declaration.params.size() == 1 ? "" : "s");
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
            for (const ast::GenericParam& candidate : impl.params) {
              if (candidate.name.name == path.segments[0].name) {
                direct = true;
                param = candidate.name.name;
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
      if (generic_impl && type_mentions_params(ast, arg, impl.params)) {
        // `&T` and friends: the shape resolves where the target's
        // arguments bind the parameters.
        spec_arg_shapes.push_back(
            SpecTarget::Arg{false, {}, error_type(), arg});
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
  // The target is a nominal type in scope, or one scalar primitive from
  // a staged package. v1 never implements a spec for a compound,
  // reference, or `str` target.
  NominalEntry* target_entry = nullptr;
  bool primitive_target = false;
  ir::TypeIdx primitive = ir::TypeIdx::invalid();
  std::string_view target_display;
  if (target_node.kind == ast::TypeKind::Primitive) {
    // A scalar belongs to the staged suite (ADR-0090): only a staged
    // package implements a spec for one, sealed or not (ADR-0133). The
    // seal above still governs which package may implement a sealed
    // spec, for a scalar as for a nominal.
    if (!tree.modules[module]->is_staged) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecRequiresNominalTarget>(
          DiagCode::UnknownType, target_node.span);
      (void)index;
      return;
    }
    primitive =
        primitive_type(target_node.payload.get<ast::TypePrimitive>().primitive,
                       target_node.span);
    if (is_error(primitive)) {
      return;
    }
    switch (tag_of(primitive)) {
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
      case ir::TypeTag::F64: break;
      default: {
        const u32 index =
            bag.emit<i18n::Key::AnalyzerSpecRequiresNominalTarget>(
                DiagCode::UnknownType, target_node.span);
        (void)index;
        return;
      }
    }
    primitive_target = true;
    target_display = pretty_tag(tag_of(primitive));
  } else if (target_node.kind == ast::TypeKind::Str) {
    // `str` is a builtin type rather than a scalar, but a sealed spec
    // may target it: byte comparison is the core suite's to give.
    if (!sealed) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecRequiresNominalTarget>(
          DiagCode::UnknownType, target_node.span);
      (void)index;
      return;
    }
    primitive = builder.primitive(ir::TypeTag::Str);
    primitive_target = true;
    target_display = pretty_tag(ir::TypeTag::Str);
  } else if (target_node.kind == ast::TypeKind::Path) {
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
          if (target_entry != nullptr &&
              !check_item_access(module, target_entry->module,
                                 ast.items[target_entry->item].is_pub,
                                 target_node.span, target_name)) {
            return;
          }
        }
      }
    }
  }
  if (target_entry == nullptr && !primitive_target) {
    const u32 index = bag.emit<i18n::Key::AnalyzerSpecRequiresNominalTarget>(
        DiagCode::UnknownType, target_node.span);
    (void)index;
    return;
  }
  // ADR-0090: an implementation lives where the type does or where the
  // spec does. A builtin type has no declaring package: it belongs to
  // the staged suite, and a package outside it cannot implement for
  // one.
  if (primitive_target) {
    if (!tree.modules[module]->is_staged) {
      const u32 index = bag.emit<i18n::Key::AnalyzerBuiltinImplOutsideItsSuite>(
          DiagCode::ImplOutsideItsHome, target_node.span, target_display);
      (void)index;
      return;
    }
  } else if (package_of(module) != package_of(target_entry->module) &&
             package_of(module) != package_of(spec_entry->module)) {
    const u32 index = bag.emit<i18n::Key::AnalyzerImplOutsideItsHome>(
        DiagCode::ImplOutsideItsHome, spec_node.span, target_entry->name,
        package_of(target_entry->module), spec_entry->name,
        package_of(spec_entry->module));
    (void)index;
    return;
  }
  SpecTarget target;
  ir::TypeIdx self_type = error_type();
  if (primitive_target) {
    target.is_primitive = true;
    target.primitive = primitive;
    self_type = primitive;
  } else {
    target.nominal = nominal_index(target_entry);
    const ast::TypePath& target_path = target_node.payload.get<ast::TypePath>();
    if (target_path.args.size() != nominal_params(*target_entry).size()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerTypeArityMismatch>(
          DiagCode::ArityMismatch, target_node.span, target_entry->name,
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
        ast::TypeIdx annotation = ast::TypeIdx::invalid();
        if (arg_node.kind == ast::TypeKind::Path) {
          const ast::Path& path =
              ast.paths[arg_node.payload.get<ast::TypePath>().path];
          if (path.segments.size() == 1 &&
              arg_node.payload.get<ast::TypePath>().args.empty()) {
            for (const ast::GenericParam& candidate : impl.params) {
              if (candidate.name.name == path.segments[0].name) {
                direct = true;
                param = candidate.name.name;
                annotation = is_value_param(ast, candidate)
                                 ? candidate.type
                                 : ast::TypeIdx::invalid();
                break;
              }
            }
          }
        }
        if (!direct) {
          const u32 index = bag.emit<i18n::Key::AnalyzerSpecTargetNotDirect>(
              DiagCode::GenericArguments, arg_node.span);
          (void)index;
          return;
        }
        if (annotation.is_valid()) {
          target.args.push_back(SpecTarget::Arg{false, param, error_type(),
                                                ast::TypeIdx::invalid(), true,
                                                annotation});
        } else {
          target.args.push_back(SpecTarget::Arg{true, param, error_type()});
        }
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
          DiagCode::DuplicateDefinition, node.span, spec_entry->name,
          target_entry != nullptr ? target_entry->name : target_display);
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
    bool declared = false;
    bool arity_ok = true;
    for (const ast::SpecMethod& method : declaration.methods) {
      if (method.name.name != name) {
        continue;
      }
      declared = true;
      arity_ok = method.generic.size() ==
                 method_node.payload.get<ast::ItemFn>().generic.size();
      break;
    }
    if (!declared) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecUnknownMethod>(
          DiagCode::UnknownValue,
          method_node.payload.get<ast::ItemFn>().name.span, name,
          spec_entry->name);
      (void)index;
      return;
    }
    // The declaration is the check: a method that declares type
    // parameters is implemented by one that declares as many.
    if (!arity_ok) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecSignatureMismatch>(
          DiagCode::TypeMismatch, method_node.span, name, spec_entry->name);
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
          DiagCode::ArityError, node.span, method.name.name, spec_entry->name);
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
  // A method with type parameters of its own has no signature until a
  // call names them, so it registers nothing below: the call's explicit
  // arguments resolve it through the generic method registry. The
  // declaration is still the check, and the two sides compare under one
  // anonymous stand-in per parameter, so neither side's names matter.
  for (ast::ItemIdx method_item : impl.methods) {
    const ast::ItemFn& method_fn =
        ast.items[method_item].payload.get<ast::ItemFn>();
    if (method_fn.generic.empty()) {
      continue;
    }
    std::vector<ir::TypeIdx> stand_ins;
    stand_ins.reserve(method_fn.generic.size());
    for (usize i = 0; i < method_fn.generic.size(); ++i) {
      stand_ins.push_back(method_param_stand_in());
    }
    std::vector<ir::TypeIdx> declared_params;
    ir::TypeIdx declared_ret = error_type();
    CheckedModule::ReceiverKind declared_receiver =
        CheckedModule::ReceiverKind::None;
    if (!spec_method_sig(spec_index, method_fn.name.name, spec_args, self_type,
                         module, stand_ins, declared_params, declared_ret,
                         declared_receiver)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecUnknownMethod>(
          DiagCode::UnknownValue, method_fn.name.span, method_fn.name.name,
          spec_entry->name);
      (void)index;
      return;
    }
    const usize pushed = type_params.size();
    for (usize i = 0; i < method_fn.generic.size(); ++i) {
      type_params.emplace_back(method_fn.generic[i].name.name, stand_ins[i]);
    }
    std::vector<ir::TypeIdx> params;
    for (const ast::ItemFnParam& param : method_fn.params) {
      params.push_back(resolve_type(module, param.type, &self_type));
    }
    ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
    if (method_fn.return_type.is_valid()) {
      ret = resolve_type(module, method_fn.return_type, &self_type);
    }
    CheckedModule::ReceiverKind receiver = CheckedModule::ReceiverKind::None;
    if (!params.empty()) {
      receiver = classify_receiver(params[0], self_type);
    }
    while (type_params.size() > pushed) {
      type_params.pop_back();
    }
    bool matches = params.size() == declared_params.size() &&
                   receiver == declared_receiver &&
                   types_equal(ret, declared_ret);
    for (usize i = 0; matches && i < params.size(); ++i) {
      matches = types_equal(params[i], declared_params[i]);
    }
    if (!matches) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecSignatureMismatch>(
          DiagCode::TypeMismatch, ast.items[method_item].span,
          method_fn.name.name, spec_entry->name);
      (void)index;
      return;
    }
  }
  for (ast::ItemIdx method_item : impl.methods) {
    const ast::ItemNode& method_node = ast.items[method_item];
    const std::string_view name =
        method_node.payload.get<ast::ItemFn>().name.name;
    if (!method_node.payload.get<ast::ItemFn>().generic.empty()) {
      continue;
    }
    std::vector<ir::TypeIdx> declared_params;
    ir::TypeIdx declared_ret = error_type();
    CheckedModule::ReceiverKind declared_receiver =
        CheckedModule::ReceiverKind::None;
    if (!spec_method_sig(spec_index, name, spec_args, self_type, module, {},
                         declared_params, declared_ret, declared_receiver)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerSpecUnknownMethod>(
          DiagCode::UnknownValue,
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
          DiagCode::TypeMismatch, method_node.span, name, spec_entry->name);
      (void)index;
      return;
    }
    CheckedModule::FnSig& sig =
        add_function(module, {name, params, ret, method_item});
    sig.is_method = true;
    add_method(module, {self_type,
                        name,
                        sig.params,
                        sig.ret,
                        receiver,
                        method_item,
                        false,
                        spec_index,
                        {}});
  }
  spec_impls.push_back(SpecImplEntry{spec_index, module, std::move(target),
                                     std::move(spec_arg_shapes), item,
                                     node.span});
}

SpecEntry* Checker::spec_of_impl_path(u32 module, ast::TypeIdx spec_side) {
  const ast::TypeNode& node = ast.types[spec_side];
  if (node.kind != ast::TypeKind::Path) {
    return nullptr;
  }
  const ast::TypePath& path = node.payload.get<ast::TypePath>();
  const std::span<const ast::Ident> segments = ast.paths[path.path].segments;
  if (segments.empty()) {
    return nullptr;
  }
  if (segments.size() == 1) {
    return find_spec_in_scope(module, segments.back().name);
  }
  u32 spec_module = NO_MODULE;
  std::string_view spec_name;
  if (!resolve_type_path(module, path.path, spec_module, spec_name)) {
    return nullptr;
  }
  return find_spec(spec_module, spec_name);
}

// A closure literal's nominal type: a one-field struct over the
// environment tuple, so the value owns its captures, its `Copy`-ness is
// the environment's, and the drop pass ends it through the field
// (ADR-0064). A literal gets its own type, because the code a value
// calls is the literal's.

bool Checker::match_compound_target(
    u32 module,
    ast::TypeIdx target,
    std::span<const ast::GenericParam> params,
    ir::TypeIdx self,
    std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
    std::vector<ValueParam>& value_scope) {
  const ast::TypeNode& node = ast.types[target.idx];
  // The walk recurses into resolutions that intern, which can move the
  // table this node was read from, so it is a copy and not a
  // reference, and so are the shapes its cases read.
  const ir::TypeNode actual = builder.types()[self];
  switch (node.kind) {
    case ast::TypeKind::Str: return actual.tag == ir::TypeTag::Str;
    case ast::TypeKind::Slice: {
      if (actual.tag != ir::TypeTag::Slice) {
        return false;
      }
      const ir::SliceType slice = builder.slice_types()[actual.as_slice()];
      return match_compound_target(module,
                                   node.payload.get<ast::TypeSlice>().element,
                                   params, slice.element, scope, value_scope);
    }
    case ast::TypeKind::Array: {
      if (actual.tag != ir::TypeTag::Array) {
        return false;
      }
      const ast::TypeArray& array = node.payload.get<ast::TypeArray>();
      const ir::ArrayType shape = builder.array_types()[actual.as_array()];
      if (array.length_path.is_valid()) {
        const ast::Path& path = ast.paths[array.length_path];
        if (path.segments.size() != 1) {
          return false;
        }
        const std::string_view name = path.segments[0].name;
        bool bound = false;
        for (const ast::GenericParam& param : params) {
          if (param.name.name == name && is_value_param(ast, param)) {
            value_scope.push_back(
                ValueParam{name, shape.count, module, param.type});
            bound = true;
            break;
          }
        }
        if (!bound) {
          return false;
        }
      } else if (array.count != shape.count) {
        return false;
      }
      return match_compound_target(module, array.element, params, shape.element,
                                   scope, value_scope);
    }
    case ast::TypeKind::Path: {
      const ast::TypePath& path = node.payload.get<ast::TypePath>();
      if (path.args.empty() && ast.paths[path.path].segments.size() == 1) {
        const std::string_view name = ast.paths[path.path].segments[0].name;
        for (const ast::GenericParam& param : params) {
          if (param.name.name == name && !is_value_param(ast, param)) {
            scope.emplace_back(name, self);
            return true;
          }
        }
      }
      const ir::TypeIdx resolved = resolve_type(module, target, nullptr);
      return !is_error(resolved) && types_equal(resolved, self);
    }
    default: return false;
  }
}

void Checker::check_spec_impl_bodies(u32 module, ast::ItemIdx item) {
  const ast::ItemImpl& impl = ast.items[item].payload.get<ast::ItemImpl>();
  for (const SpecImplEntry& entry : spec_impls) {
    if (entry.item != item) {
      continue;
    }
    ir::TypeIdx self_type = error_type();
    if (entry.target.is_primitive) {
      // A scalar target has no arguments; its methods were registered
      // against the scalar itself.
      self_type = entry.target.primitive;
    } else {
      std::vector<ir::TypeIdx> args;
      for (const SpecTarget::Arg& arg : entry.target.args) {
        if (arg.is_param || arg.is_value || arg.shape.is_valid()) {
          return;
        }
        args.push_back(arg.type);
      }
      if (args.empty()) {
        self_type = intern_nominal(nominals[entry.target.nominal]);
      } else {
        self_type = instantiate_generic(entry.target.nominal, args, entry.span);
      }
      if (is_error(self_type)) {
        return;
      }
    }
    spec_scope.push_back(entry.spec);
    for (ast::ItemIdx method_item : impl.methods) {
      // A method that declares its own type parameters is checked per
      // instantiation, where the call spells them.
      if (!ast.items[method_item].payload.get<ast::ItemFn>().generic.empty()) {
        continue;
      }
      // A method a const initializer already reached had its body typed
      // on demand.
      if (!constInitTables_.prechecked_fns_.contains(method_item.idx)) {
        check_fn(module, method_item, &self_type);
      }
    }
    spec_scope.pop_back();
    return;
  }
}

}  // namespace analyzer
