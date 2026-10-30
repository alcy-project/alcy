// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/types.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/checker.h"
#include "analyzer/comp_evaluator.h"
#include "analyzer/diag_code.h"
#include "analyzer/fmt.h"
#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "ast/verify.h"
#include "base/nesting.h"
#include "comp/comp_value.h"
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
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "ir/verifier.h"

namespace analyzer {

// The key a witness is recorded under: the instantiation's type
// arguments with the witness slot itself left open, so the two sides of
// a lookup agree whichever stand-in the other side used.
std::vector<ir::TypeIdx> witness_key(std::span<const ir::TypeIdx> args,
                                     usize slot) {
  std::vector<ir::TypeIdx> key(args.begin(), args.end());
  if (slot < key.size()) {
    key[slot] = ir::TypeIdx::invalid();
  }
  return key;
}

Checker::Checker(const ModuleTree& tree,
                 ir::PointerWidth width,
                 ast::AstArena& ast,
                 diag::DiagBag& bag,
                 ir::SymbolTable& strings,
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
      const bool staged = tree.modules[m]->is_staged;
      if (!duplicate &&
          (name == "MaybeUninit" || (name == "Anchor" && !staged))) {
        // The wrapper is compiler-owned; a declaration under the same
        // name would make the spelling resolve two ways.
        const u32 index = bag.emit<i18n::Key::AnalyzerBuiltinType>(
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
            DiagCode::ReservedName, span, name);
        (void)index;
        continue;
      }
      if (duplicate) {
        const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
            DiagCode::DuplicateDefinition, span, name);
        (void)index;
        continue;
      }
      nominals.push_back(NominalEntry{m, name, item, span, ir::TypeIdx(0)});
      nominals_of_module[m].push_back(static_cast<u32>(nominals.size() - 1));
    }
  }
}

// A value parameter's annotation fixes the kind of value its arguments
// carry, so it must name an integer type. The annotation may name a
// previously registered nominal, so this runs with the whole table
// filled.
void Checker::record_comp_call(void* context, const CompCallRecord& record) {
  static_cast<Checker*>(context)->comp_calls_.push_back(record);
}

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

bool Checker::is_param_value(ir::TypeIdx type) const {
  ir::TypeIdx inner = type_origin(type);
  const ir::TypeTag tag = tag_of(inner);
  if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    inner = builder.ref_types()[builder.types()[inner.idx].as_ref()].pointee;
  }
  return tag_of(inner) == ir::TypeTag::Param;
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

std::string_view Checker::type_name(ir::TypeIdx type) const {
  const ir::TypeIdx origin = type_origin(type);
  const ir::TypeTag tag = tag_of(origin);
  if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Enum) {
    if (const GenericInstance* instance = generic_find(origin)) {
      return nominals[instance->nominal].name;
    }
    const auto declaring = nominal_by_type_.find(origin.idx);
    if (declaring != nominal_by_type_.end()) {
      return nominals[declaring->second].name;
    }
  }
  return pretty_tag(tag);
}

std::string Checker::type_display(ir::TypeIdx type) const {
  return type_display_at(type, 0);
}

std::string Checker::type_display_at(ir::TypeIdx type, usize depth) const {
  if (is_error(type)) {
    return "error";
  }
  if (depth > 6) {
    return "...";
  }
  const ir::TypeIdx origin = type_origin(type);
  const ir::TypeNode& node = builder.types()[origin];
  switch (node.tag) {
    case ir::TypeTag::Ref:
      return "&" + type_display_at(builder.ref_types()[node.as_ref()].pointee,
                                   depth + 1);
    case ir::TypeTag::MutRef:
      return "&mut " +
             type_display_at(builder.ref_types()[node.as_ref()].pointee,
                             depth + 1);
    case ir::TypeTag::RawPtr:
      return "*" + type_display_at(builder.ref_types()[node.as_ref()].pointee,
                                   depth + 1);
    case ir::TypeTag::RawMutPtr:
      return "*mut " +
             type_display_at(builder.ref_types()[node.as_ref()].pointee,
                             depth + 1);
    case ir::TypeTag::Array: {
      const ir::ArrayType& array = builder.array_types()[node.as_array()];
      return "[" + type_display_at(array.element, depth + 1) + "; " +
             std::to_string(array.count) + "]";
    }
    case ir::TypeTag::Slice: {
      const ir::SliceType& slice = builder.slice_types()[node.as_slice()];
      return "[" + type_display_at(slice.element, depth + 1) + "]";
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType& tuple = builder.tuple_types()[node.as_tuple()];
      std::string out = "(";
      for (usize i = 0; i < tuple.elements.size(); ++i) {
        if (i != 0) {
          out += ", ";
        }
        out += type_display_at(tuple.elements[i], depth + 1);
      }
      out += ')';
      return out;
    }
    case ir::TypeTag::Func: {
      const ir::FuncType& func = builder.func_types()[node.as_func()];
      std::string out = "(";
      for (usize i = 0; i < func.params.size(); ++i) {
        if (i != 0) {
          out += ", ";
        }
        out += type_display_at(func.params[i], depth + 1);
      }
      out += ") -> " + type_display_at(func.ret, depth + 1);
      return out;
    }
    case ir::TypeTag::Struct: {
      const ir::StructType& shape = builder.struct_types()[node.as_struct()];
      // The shape carries its declared name, which a nominal lookup can
      // miss when the type only appears as another instance's argument.
      const std::string_view declared = interner.get(shape.name);
      std::string out(declared.empty() ? type_name(origin) : declared);
      if (shape.params.size() > 0) {
        out += '<';
        for (usize i = 0; i < shape.params.size(); ++i) {
          if (i != 0) {
            out += ", ";
          }
          out += type_display_at(shape.params[i], depth + 1);
        }
        out += '>';
      }
      return out;
    }
    case ir::TypeTag::Enum: {
      const ir::EnumType& shape = builder.enum_types()[node.as_enum()];
      const std::string_view declared = interner.get(shape.name);
      std::string out(declared.empty() ? type_name(origin) : declared);
      if (shape.params.size() > 0) {
        out += '<';
        for (usize i = 0; i < shape.params.size(); ++i) {
          if (i != 0) {
            out += ", ";
          }
          out += type_display_at(shape.params[i], depth + 1);
        }
        out += '>';
      }
      return out;
    }
    case ir::TypeTag::Const:
      return std::to_string(builder.const_types()[node.as_const()].value);
    case ir::TypeTag::Param: {
      // The name the declaration wrote, while the definition-site
      // check's scope is in; the index is what remains otherwise.
      const ast::GenericParam* declared = param_at(node.as_param().value);
      if (declared != nullptr) {
        return std::string(declared->name.name);
      }
      return "T" + std::to_string(node.as_param().value);
    }
    default: return std::string(pretty_tag(node.tag));
  }
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
NominalEntry* Checker::builtin_nominal(std::string_view name) {
  for (NominalEntry& entry : nominals) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

// Whether a type expression names any of these parameters, directly or
// under a constructor. A spec argument that does is stored as a shape
// and resolved where the target's arguments bind them.
static bool type_mentions_params(const ast::AstArena& ast,
                                 ast::TypeIdx type,
                                 std::span<const ast::GenericParam> params) {
  const ast::TypeNode& node = ast.types[type.idx];
  switch (node.kind) {
    case ast::TypeKind::Path: {
      const ast::TypePath& path = node.payload.get<ast::TypePath>();
      if (path.path.is_valid() && ast.paths[path.path].segments.size() == 1) {
        for (const ast::GenericParam& param : params) {
          if (param.name.name == ast.paths[path.path].segments[0].name) {
            return true;
          }
        }
      }
      for (ast::TypeIdx arg : path.args) {
        if (type_mentions_params(ast, arg, params)) {
          return true;
        }
      }
      return false;
    }
    case ast::TypeKind::Tuple: {
      for (ast::TypeIdx element : node.payload.get<ast::TypeTuple>().elements) {
        if (type_mentions_params(ast, element, params)) {
          return true;
        }
      }
      return false;
    }
    case ast::TypeKind::Array:
      return type_mentions_params(
          ast, node.payload.get<ast::TypeArray>().element, params);
    case ast::TypeKind::Slice:
      return type_mentions_params(
          ast, node.payload.get<ast::TypeSlice>().element, params);
    case ast::TypeKind::Ref:
      return type_mentions_params(ast, node.payload.get<ast::TypeRef>().inner,
                                  params);
    case ast::TypeKind::RawPtr:
      return type_mentions_params(
          ast, node.payload.get<ast::TypeRawPtr>().inner, params);
    case ast::TypeKind::Func: {
      const ast::TypeFunc& fn = node.payload.get<ast::TypeFunc>();
      for (ast::TypeIdx param : fn.params) {
        if (type_mentions_params(ast, param, params)) {
          return true;
        }
      }
      return fn.ret.is_valid() && type_mentions_params(ast, fn.ret, params);
    }
    case ast::TypeKind::OwnFunc: {
      // Shares the payload; a parameter nested under an owning
      // spelling still mentions its slot.
      const ast::TypeFunc& fn = node.payload.get<ast::TypeFunc>();
      for (ast::TypeIdx param : fn.params) {
        if (type_mentions_params(ast, param, params)) {
          return true;
        }
      }
      return fn.ret.is_valid() && type_mentions_params(ast, fn.ret, params);
    }
    default: return false;
  }
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
    // package implements a spec for one, sealed or not (ADR-0122). The
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
  const ir::TypeNode node = builder.types()[first];
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

// One anonymous nominal per method type parameter, so a declaration
// and an implementation compare without either side's names: the same
// stand-in binds both. Each call makes a fresh type, so no two
// parameters are ever the same.
ir::TypeIdx Checker::method_param_stand_in() {
  ir::TypeSeq fields;
  return builder.struct_type(method_param_name_id, fields.finish(),
                             ir::TypeIdxRange{});
}

ir::TypeIdx Checker::own_fn_return_stand_in() {
  ir::TypeSeq fields;
  return builder.struct_type(own_fn_return_stand_in_id, fields.finish(),
                             ir::TypeIdxRange{});
}

// The spec a spec side names, through the same type grammar an impl's
// spec side parses with. Registration reports what it needs; resolving
// a method's own entry only wants the index.
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
ir::TypeIdx Checker::intern_closure(ir::TypeIdx env) {
  return builder.struct_type(
      closure_name_id,
      [&] {
        ir::TypeSeq seq;
        seq.push(storage_copy(env));
        return seq.finish();
      }(),
      ir::TypeIdxRange{});
}

// The environment tuple a literal's captures lay out: a moved capture
// holds the value, a borrow holds the reference, in list order.
ir::TypeIdx Checker::closure_env_type(std::span<const CaptureEntry> captures) {
  std::vector<ir::TypeIdx> fields;
  fields.reserve(captures.size());
  for (const CaptureEntry& entry : captures) {
    fields.push_back(
        entry.mode == ast::CaptureMode::Move
            ? entry.type
            : builder.reference_type(entry.type,
                                     entry.mode == ast::CaptureMode::Mut));
  }
  // A tuple's element range must be consecutive slots, so every field
  // type is resolved first and only then appended as a copy, the way
  // the lowering builds the same tuple: interning a reference between
  // appends would break the run.
  ir::TypeSeq seq;
  for (ir::TypeIdx field : fields) {
    seq.push(builder.ref_type(field));
  }
  return builder.tuple_type(seq.finish());
}

const Checker::ClosureSig* Checker::closure_sig_of_type(
    ir::TypeIdx type) const {
  for (const ClosureSig& sig : closure_sigs) {
    if (sig.self_type.idx == type.idx) {
      return &sig;
    }
  }
  return nullptr;
}

ir::TypeIdx Checker::uninit_payload(ir::TypeIdx type) const {
  if (tag_of(type) != ir::TypeTag::Struct) {
    return ir::TypeIdx::invalid();
  }
  const ir::StructType shape =
      builder.struct_types()[builder.types()[type.idx].as_struct()];
  if (shape.name != uninit_name_id) {
    return ir::TypeIdx::invalid();
  }
  return shape.fields.size() == 1 ? shape.fields[0] : ir::TypeIdx::invalid();
}

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
        DiagCode::NameTableExhausted, diag::Span{});
    (void)index;
  }
  return str::INVALID_STRING_POOL_ID;
}

// Interns a registered nominal, reserving its index first so recursive
// references resolve to it. A post-pass rejects uninhabited cycles.
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
  // The anchor is the one type a declaration makes unmovable; anything
  // holding one derives that (ADR-0085).
  if (entry.name == "Anchor" && tree.modules[entry.module]->is_staged) {
    if (not_movable_.size() <= entry.type.idx) {
      not_movable_.resize(entry.type.idx + 1, 0);
    }
    not_movable_[entry.type.idx] = 1;
  }
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
  // A copy stands for the declaration it came from, so it carries the
  // mark too: a walk over field slots sees copies (ADR-0085).
  if (type.idx < not_movable_.size() && not_movable_[type.idx] != 0) {
    if (not_movable_.size() <= copy.idx) {
      not_movable_.resize(copy.idx + 1, 0);
    }
    not_movable_[copy.idx] = 1;
  }
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

std::span<const ast::GenericParam> Checker::nominal_params(
    const NominalEntry& entry) {
  const ast::ItemNode& node = ast.items[entry.item];
  if (node.kind == ast::ItemKind::Struct) {
    return node.payload.get<ast::ItemStruct>().params;
  }
  return node.payload.get<ast::ItemEnum>().params;
}

Checker::GenericScope Checker::push_generic_scope(
    const GenericInstance& instance) {
  const GenericScope kept{type_params.size(), value_params.size()};
  const NominalEntry& entry = nominals[instance.nominal];
  const std::span<const ast::GenericParam> params = nominal_params(entry);
  for (usize i = 0; i < params.size(); ++i) {
    if (is_value_param(ast, params[i])) {
      value_params.push_back({params[i].name.name,
                              const_type_value(instance.args[i]), entry.module,
                              params[i].type});
    } else {
      type_params.emplace_back(params[i].name.name, instance.args[i]);
    }
  }
  return kept;
}

void Checker::pop_generic_scope(const GenericScope& kept) {
  while (type_params.size() > kept.type_params) {
    type_params.pop_back();
  }
  while (value_params.size() > kept.value_params) {
    value_params.pop_back();
  }
}

const Checker::ValueParam* Checker::value_param(std::string_view name) const {
  for (usize i = value_params.size(); i > 0; --i) {
    if (value_params[i - 1].name == name) {
      return &value_params[i - 1];
    }
  }
  return nullptr;
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

// Whether a path head is one the module walk owns: the two relative
// forms and the two neighbor-language spellings that get their own
// message (ADR-0060).
bool module_walk_head(std::string_view head) {
  return head == "package" || head == "self" || head == "super" ||
         head == "crate";
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
    // The root of the package this module belongs to, not the goal:
    // each package's modules hang under their own root (ADR-0060).
    current = module;
    while (parents[current] != NO_MODULE) {
      current = parents[current];
    }
  } else if (head == "self") {
    current = module;
  } else {
    const auto root = root_by_identity_.find(head);
    if (root != root_by_identity_.end()) {
      current = tree.package_roots[root->second].module;
      opened_root = root->second;
    }
    if (opened_root == analyzer::NO_PACKAGE_ROOT) {
      current = find_child_module(module, head);
      if (current == NO_MODULE) {
        if (head == "super" || head == "crate") {
          const u32 index = bag.emit<i18n::Key::AnalyzerPathHeadNotSupported>(
              DiagCode::SpellingSuggestion, segments[0].span, head);
          (void)index;
          return false;
        }
        const u32 index = bag.emit<i18n::Key::AnalyzerUnresolved>(
            DiagCode::UnknownType, ast.paths[path].span, what, head);
        (void)index;
        return false;
      }
    }
  }
  for (usize i = 1; i + 1 < segments.size(); ++i) {
    current = find_child_module(current, segments[i].name);
    if (current == NO_MODULE) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnresolved>(
          DiagCode::UnknownType, ast.paths[path].span, what, segments[i].name);
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

// Resolves one argument at a value-declared generic slot. A literal is
// its own value; a path names a visible value parameter first, then a
// `const` item; anything else is a type where the declaration requires
// a value.
bool Checker::resolve_value_argument(u32 module,
                                     ast::TypeIdx arg,
                                     std::string_view param,
                                     diag::Span span,
                                     u64& out) {
  const auto reject_expected = [&] {
    const u32 index = bag.emit<i18n::Key::AnalyzerValueArgumentExpected>(
        DiagCode::TypeMismatch, span, param);
    (void)index;
    return false;
  };
  const ast::TypeNode& node = ast.types[arg];
  if (node.kind == ast::TypeKind::Value) {
    const ast::Literal& literal =
        ast.literals[node.payload.get<ast::TypeValue>().literal];
    if (literal.is_negative) {
      const u32 index = bag.emit<i18n::Key::AnalyzerValueArgumentNotInteger>(
          DiagCode::InvalidOperation, span);
      (void)index;
      return false;
    }
    out = comp::parse_numeric_value(literal.spelling);
    return true;
  }
  if (node.kind != ast::TypeKind::Path ||
      !node.payload.get<ast::TypePath>().args.empty()) {
    return reject_expected();
  }
  const ast::PathIdx path_idx = node.payload.get<ast::TypePath>().path;
  const ast::Path& path = ast.paths[path_idx];
  if (path.segments.empty()) {
    return reject_expected();
  }
  if (path.segments.size() == 1) {
    if (const ValueParam* bound = value_param(path.segments[0].name)) {
      out = bound->value;
      return true;
    }
  }
  u32 target = NO_MODULE;
  if (!walk_module_prefix(module, path_idx, "value", target)) {
    return false;
  }
  const std::string_view member = path.segments.back().name;
  if (const CheckedModule::StaticInfo* info = lookup_static(target, member)) {
    if (!check_item_access(module, info->module, info->is_pub, span, member)) {
      return false;
    }
    if (!info->is_const || !info->init.is_valid()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerValueArgumentNotInteger>(
          DiagCode::InvalidOperation, span);
      (void)index;
      return false;
    }
    const CheckedModule::StaticInfo definition = *info;
    return const_int_value<i18n::Key::AnalyzerValueArgumentNotInteger>(
        definition, span, out);
  }
  // A single name that is an in-scope type is the case a declaration
  // spells with a value slot by mistake; name the slot rather than
  // reporting an unresolved value.
  if (path.segments.size() == 1 &&
      find_nominal_in_scope(module, member) != nullptr) {
    return reject_expected();
  }
  emit_unresolved(bag, DiagCode::UnknownValue, span, std_hints, "value",
                  member);
  return false;
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
  // Owning function types parse but wait for their slice (ADR-0075):
  // refuse them here rather than let one reach lowering as a view.
  if (node.kind == ast::TypeKind::OwnFunc) {
    const u32 index = bag.emit<i18n::Key::AnalyzerOwnFunctionUnsupported>(
        DiagCode::InvalidOperation, node.span);
    (void)index;
    return error_type();
  }
  switch (node.kind) {
    case ast::TypeKind::Value: {
      // A literal is parsed in argument lists only; reaching a type
      // position means the declaration takes a type there.
      const u32 index = bag.emit<i18n::Key::AnalyzerValueCannotBeType>(
          DiagCode::TypeMismatch, node.span);
      (void)index;
      return error_type();
    }
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
      u64 count = array.count;
      if (array.length_path.is_valid() &&
          !const_array_length(module, array.length_path, node.span, count)) {
        return error_type();
      }
      return builder.array_type(element, count);
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
            DiagCode::UnknownType, node.span);
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
    case ast::TypeKind::OwnFunc: {
      // Unreachable: the guard above refuses owning spellings before
      // the switch. Lowering never sees one as a view.
      return error_type();
    }
    case ast::TypeKind::Slice: {
      const ast::TypeSlice& slice = node.payload.get<ast::TypeSlice>();
      const ir::TypeIdx element = resolve_type(module, slice.element, self);
      if (!behind_ref) {
        const u32 index =
            bag.emit<i18n::Key::AnalyzerUnsizedTypeNeedsReference>(
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
                DiagCode::UnknownType, node.span);
            (void)index;
            return error_type();
          }
          if (!node.payload.get<ast::TypePath>().args.empty()) {
            const u32 index =
                bag.emit<i18n::Key::AnalyzerGenericArgumentsUnsupported>(
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
                      DiagCode::GenericArguments, node.span);
              (void)index;
              return error_type();
            }
            return type_params[i - 1].second;
          }
        }
        // A value parameter names no type, whatever else is in scope.
        if (value_param(name) != nullptr) {
          const u32 index = bag.emit<i18n::Key::AnalyzerValueParameterAsType>(
              DiagCode::UnknownType, node.span, name);
          (void)index;
          return error_type();
        }
        if (name == "MaybeUninit") {
          const auto& args = node.payload.get<ast::TypePath>().args;
          if (args.size() != 1) {
            const u32 index = bag.emit<i18n::Key::AnalyzerMaybeUninitArity>(
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
              DiagCode::UnknownType, node.span, target_name, target_name,
              package);
          (void)index;
          return error_type();
        }
        const u32 index = bag.emit<i18n::Key::AnalyzerUnknownType>(
            DiagCode::UnknownType, node.span, target_name);
        (void)index;
        return error_type();
      }
      if (!check_item_access(module, entry->module,
                             ast.items[entry->item].is_pub, node.span,
                             target_name)) {
        return error_type();
      }
      // The checker resolved this name; record where it pointed, so
      // navigation reads the resolution instead of repeating it.
      modules[module].references.push_back({node.span, entry->span});
      const usize param_count = nominal_params(*entry).size();
      const usize arg_count = node.payload.get<ast::TypePath>().args.size();
      if (param_count == 0) {
        if (arg_count != 0) {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerGenericArgumentsUnsupported>(
                  DiagCode::GenericArguments, node.span);
          (void)index;
          return error_type();
        }
        return intern_nominal(*entry);
      }
      if (arg_count != param_count) {
        const u32 index = bag.emit<i18n::Key::AnalyzerArityMismatch>(
            DiagCode::ArityMismatch, node.span, target_name, param_count,
            param_count == 1 ? "" : "s");
        (void)index;
        return error_type();
      }
      const std::span<const ast::GenericParam> params = nominal_params(*entry);
      std::vector<ir::TypeIdx> args;
      args.reserve(arg_count);
      for (usize i = 0; i < arg_count; ++i) {
        const ast::TypeIdx arg = node.payload.get<ast::TypePath>().args[i];
        if (is_value_param(ast, params[i])) {
          u64 value = 0;
          if (!resolve_value_argument(module, arg, params[i].name.name,
                                      node.span, value)) {
            return error_type();
          }
          args.push_back(builder.const_type(value));
        } else {
          args.push_back(resolve_type(module, arg, self));
        }
      }
      const u32 nominal = static_cast<u32>(entry - nominals.data());
      return instantiate_generic(nominal, args, node.span);
    }
  }
}

// Closed compiler-known intrinsic set (see docs/spec/items.md).
// `panic` stays callable without a declaration; `print`/`println` are
// declared by io and expanded by package (ADR-0055); `memcopy`
// requires a declaration.
bool Checker::is_known_intrinsic(std::string_view name) {
  return name == "memcopy" || name == "panic" || name == "str_len" ||
         name == "str_byte" || name == "str_slice" || name == "sys_write" ||
         name == "sys_read" || name == "str_from_parts" ||
         name == "slice_len" || name == "slice_from_parts" ||
         name == "slice_from_parts_mut" || name == "alloc" ||
         name == "dealloc" || name == "elem_ptr" || name == "elem_ref" ||
         name == "size_of" || name == "align_of" || name == "uninit_write" ||
         name == "uninit_assume" || name == "uninit_ref" ||
         name == "ptr_offset" || name == "ptr_offset_mut" ||
         name == "drop_in_place" || name == "slice_at" ||
         name == "slice_at_mut" || name == "read" || name == "wrapping_add" ||
         name == "wrapping_sub" || name == "wrapping_mul" ||
         name == "file_size" || name == "read_file_into" ||
         name == "write_file_all" || name == "arg_count" || name == "arg";
}

// The intrinsics whose preconditions the compiler cannot check
// (ADR-0050): the declaration must carry `unsafe` and a call needs
// the gate. Every other intrinsic is safe whatever its arguments.
static bool intrinsic_is_unsafe(std::string_view name) {
  return name == "alloc" || name == "dealloc" || name == "elem_ptr" ||
         name == "elem_ref" || name == "memcopy" || name == "uninit_assume" ||
         name == "str_from_parts" || name == "slice_from_parts" ||
         name == "slice_from_parts_mut" || name == "ptr_offset" ||
         name == "ptr_offset_mut" || name == "drop_in_place" ||
         name == "slice_at" || name == "slice_at_mut" || name == "read" ||
         name == "sys_read" || name == "file_size" ||
         name == "read_file_into" || name == "write_file_all" ||
         name == "arg_count" || name == "arg";
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
                  DiagCode::InvalidOperation, intrinsic.name.span, name)
            : bag.emit<i18n::Key::AnalyzerIntrinsicMissingUnsafe>(
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
  if (name == "wrapping_add" || name == "wrapping_sub" ||
      name == "wrapping_mul") {
    // `wrapping_add<T>(a: T, b: T) -> T`: both operands and the result
    // are one integer type, whatever its signedness.
    return or_wrong(params.size() == 2 && same(params[0], params[1]) &&
                    same(params[0], ret) &&
                    ir::is_integer_type(tag_of(params[0])));
  }
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
  if (name == "drop_in_place") {
    // `drop_in_place<T>(p: &mut T)`: the value behind the reference is
    // initialized and owned here, which is the gate's precondition.
    return or_wrong(params.size() == 1 &&
                    builder.types()[params[0].idx].tag == ir::TypeTag::MutRef &&
                    builder.types()[ret.idx].tag == ir::TypeTag::Void);
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
  if (name == "slice_at") {
    // `slice_at<T>(s: &[T], i: usize) -> &T`: the caller keeps `i`
    // within the slice, which is the gate's precondition.
    if (!slice_ref(params[0]) ||
        builder.types()[ret.idx].tag != ir::TypeTag::Ref) {
      return wrong();
    }
    if (tag_of(pointee(params[0])) != ir::TypeTag::Slice) {
      return wrong();
    }
    const ir::SliceType slice =
        builder.slice_types()[builder.types()[pointee(params[0])].as_slice()];
    return or_wrong(params.size() == 2 && is_usize(params[1]) &&
                    same(slice.element, pointee(ret)));
  }
  if (name == "slice_at_mut") {
    // `slice_at_mut<T>(s: &mut [T], i: usize) -> &mut T`, the exclusive
    // counterpart over the same precondition.
    if (!slice_ref(params[0]) ||
        builder.types()[ret.idx].tag != ir::TypeTag::MutRef) {
      return wrong();
    }
    if (tag_of(pointee(params[0])) != ir::TypeTag::Slice) {
      return wrong();
    }
    const ir::SliceType slice =
        builder.slice_types()[builder.types()[pointee(params[0])].as_slice()];
    return or_wrong(params.size() == 2 && is_usize(params[1]) &&
                    same(slice.element, pointee(ret)));
  }
  if (name == "read") {
    // `read<T>(p: *T) -> T`: the caller keeps the place initialized and
    // gives up ownership of what it reads.
    return or_wrong(params.size() == 1 &&
                    builder.types()[params[0].idx].tag == ir::TypeTag::RawPtr &&
                    same(pointee(params[0]), ret));
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
    const ir::SliceType slice =
        builder.slice_types()[builder.types()[pointee(ret)].as_slice()];
    const bool kinds_match =
        (ret_tag == ir::TypeTag::MutRef && ptr_tag == ir::TypeTag::MutRef) ||
        (ret_tag == ir::TypeTag::Ref && ptr_tag == ir::TypeTag::Ref);
    return or_wrong(params.size() == 2 && is_usize(params[1]) &&
                    same(slice.element, pointee(params[0])) && kinds_match);
  }
  if (name == "memcopy") {
    // `memcopy<T>(dst: &mut T, src: &T, n: usize)`: the pointees are one
    // storage type, so a buffer of any element or `MaybeUninit` slot
    // crosses while the copy still reads bytes.
    return or_wrong(params.size() == 3 && is_usize(params[2]) &&
                    builder.types()[params[0].idx].tag == ir::TypeTag::MutRef &&
                    builder.types()[params[1].idx].tag == ir::TypeTag::Ref &&
                    same(pointee(params[0]), pointee(params[1])) &&
                    builder.types()[ret.idx].tag == ir::TypeTag::Void);
  }
  if (name == "panic") {
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
    expected_ret = builder.primitive(ir::TypeTag::I64);
  } else if (name == "sys_read") {
    expected.push_back(builder.primitive(ir::TypeTag::I32));
    expected.push_back(builder.reference_type(u8, true));
    expected.push_back(usize_ty);
    expected_ret = builder.primitive(ir::TypeTag::I64);
  } else if (name == "str_from_parts") {
    expected.push_back(builder.reference_type(u8, false));
    expected.push_back(usize_ty);
    expected_ret = str;
  } else if (name == "file_size") {
    expected.push_back(str);
    expected_ret = builder.primitive(ir::TypeTag::I64);
  } else if (name == "read_file_into") {
    expected.push_back(str);
    expected.push_back(builder.reference_type(u8, true));
    expected.push_back(usize_ty);
    expected_ret = builder.primitive(ir::TypeTag::I64);
  } else if (name == "write_file_all") {
    expected.push_back(str);
    expected.push_back(str);
    expected_ret = builder.primitive(ir::TypeTag::I64);
  } else if (name == "arg_count") {
    expected_ret = builder.primitive(ir::TypeTag::I64);
  } else if (name == "arg") {
    expected.push_back(builder.primitive(ir::TypeTag::I64));
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
  const auto contains_name = [](const std::vector<std::string_view>& names,
                                std::string_view name) {
    return std::ranges::find(names, name) != names.end();
  };
  const auto duplicate_value = [this](const ast::Ident& name) {
    const u32 index = bag.emit<i18n::Key::AnalyzerDuplicateDefinition>(
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
          // A duplicate member name belongs to the declaration, not to
          // an instantiation, so the check runs before a generic form
          // interns per use: two fields under one name would leave a
          // literal looking for both and an access reaching the first,
          // and a second variant would resolve to whichever entry a
          // lookup reached first.
          std::vector<std::string_view> members;
          if (node.kind == ast::ItemKind::Struct) {
            for (const ast::ItemStructField& field :
                 node.payload.get<ast::ItemStruct>().fields) {
              if (contains_name(members, field.name.name)) {
                duplicate_value(field.name);
                continue;
              }
              members.push_back(field.name.name);
            }
          } else {
            for (const ast::ItemEnumVariant& variant :
                 node.payload.get<ast::ItemEnum>().variants) {
              if (contains_name(members, variant.name.name)) {
                duplicate_value(variant.name);
                continue;
              }
              members.push_back(variant.name.name);
            }
          }
          // Generic declarations intern per instantiation on use; the
          // bare declaration has no type of its own.
          if (!nominal_params(*entry).empty()) {
            break;
          }
          const ir::TypeIdx resolved = intern_nominal(*entry);
          modules[module].types.push_back({entry->name, resolved});
          if (node.kind == ast::ItemKind::Struct) {
            modules[module].structs.push_back(
                {entry->item, resolved, std::move(members)});
          } else {
            modules[module].enums.push_back(
                {entry->item, entry->name, resolved, std::move(members)});
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
        // sites; eager registration cannot bind their parameters. An
        // owning `fn(...)` parameter makes the item generic first
        // (ADR-0075).
        desugar_own_fn_params(item);
        register_fn_signature(module, item);
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
          const std::vector<ValueParam> kept_values = value_params;
          type_params.clear();
          value_params.clear();
          for (const ast::GenericParam& param : intrinsic.generic) {
            type_params.emplace_back(param.name.name,
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
          value_params = kept_values;
          check_intrinsic_signature(module, intrinsic, shape_params, shape_ret);
          break;
        }
        std::vector<ir::TypeIdx> params;
        for (const ast::ItemFnParam& param : intrinsic.params) {
          if (param.is_comp) {
            const u32 index =
                bag.emit<i18n::Key::AnalyzerCompParameterOnIntrinsic>(
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
                      DiagCode::InvalidComp, ast.patterns[param.pattern].span);
              (void)index;
              abi_ok = false;
              break;
            }
            const ir::TypeIdx type = resolve_type(module, param.type, nullptr);
            if (!is_error(type) && !extern_abi_type_ok(tag_of(type), false)) {
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerExternTypeNotSupported>(
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
        register_static(module, item);
        break;
      }
      case ast::ItemKind::Impl: {
        // An owning `fn(...)` parameter desugars before any method of
        // the block registers (ADR-0075); the method path infers the
        // witness from the call's arguments like a free function's.
        for (ast::ItemIdx method_item :
             node.payload.get<ast::ItemImpl>().methods) {
          desugar_own_fn_params(method_item);
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
                    DiagCode::GenericArguments, self_node.span);
            (void)index;
          } else {
            bool resolved = false;
            NominalEntry* entry =
                find_impl_target(module, node.payload.get<ast::ItemImpl>().type,
                                 target_name, &resolved);
            if (entry != nullptr) {
              target_module = entry->module;
              (void)check_item_access(module, entry->module,
                                      ast.items[entry->item].is_pub,
                                      self_node.span, target_name);
              target_known = true;
              // ADR-0090: an inherent implementation lives where the
              // type does.
              if (package_of(module) != package_of(entry->module)) {
                const u32 index =
                    bag.emit<i18n::Key::AnalyzerInherentImplOutsideItsType>(
                        DiagCode::ImplOutsideItsHome, self_node.span,
                        target_name, package_of(entry->module));
                (void)index;
                break;
              }
              if (nominal_params(*entry).empty() &&
                  node.payload.get<ast::ItemImpl>().params.empty()) {
                self_type = intern_nominal(*entry);
                self_ok = true;
              } else {
                generic_impl = true;
              }
            } else if (resolved) {
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerInherentImplNeedsNominalType>(
                      DiagCode::UnknownType, self_node.span);
              (void)index;
            }
          }
        } else if (self_node.kind == ast::TypeKind::Str) {
          // `impl str`: the string view is a builtin, and the staged
          // suite owns the methods on it (ADR-0090).
          if (!tree.modules[module]->is_staged) {
            const u32 index =
                bag.emit<i18n::Key::AnalyzerBuiltinImplOutsideItsSuite>(
                    DiagCode::ImplOutsideItsHome, self_node.span, "str");
            (void)index;
            break;
          }
          self_type = builder.primitive(ir::TypeTag::Str);
          self_ok = true;
        } else if (self_node.kind == ast::TypeKind::Slice ||
                   self_node.kind == ast::TypeKind::Array) {
          // A slice or array is a builtin too: the staged suite owns
          // the methods on one (ADR-0090).
          if (!tree.modules[module]->is_staged) {
            const u32 index =
                bag.emit<i18n::Key::AnalyzerBuiltinImplOutsideItsSuite>(
                    DiagCode::ImplOutsideItsHome, self_node.span, "[T]");
            (void)index;
            break;
          }
          if (node.payload.get<ast::ItemImpl>().params.empty()) {
            // `impl [i32]`: a concrete compound target registers like a
            // nominal's methods, keyed by the interned shape.
            self_type = resolve_type(
                module, node.payload.get<ast::ItemImpl>().type, nullptr);
            self_ok = !is_error(self_type);
          } else {
            // `impl<T> [T]`: no nominal keys it, so the block waits and
            // its shape is matched against a receiver at lookup.
            generic_impl = true;
            compound_impls_.push_back(CompoundImplEntry{module, item});
          }
        } else {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerInherentImplNeedsNominalType>(
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
          // A method with its own type parameters waits for its call
          // site, which spells them; registering now would resolve the
          // parameters without a binding.
          if (!method_node.payload.get<ast::ItemFn>().generic.empty()) {
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
                      sig.params,
                      sig.ret,
                      receiver,
                      method,
                      self_ok && check_drop_signature(module, self_type, sig,
                                                      receiver, method),
                      NO_SPEC,
                      {}});
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
        DiagCode::BadDropSignature, name.span);
    (void)index;
    return false;
  }
  const ir::TypeTag self_tag = tag_of(self_type);
  const bool view_like =
      self_tag == ir::TypeTag::Str || self_tag == ir::TypeTag::Slice ||
      self_tag == ir::TypeTag::Ref || self_tag == ir::TypeTag::MutRef ||
      self_tag == ir::TypeTag::RawPtr || self_tag == ir::TypeTag::RawMutPtr ||
      self_tag == ir::TypeTag::Func || self_tag == ir::TypeTag::Function;
  if (view_like) {
    // A view is a window on storage someone else owns; it has nothing
    // to end, so a destructor on it is refused. Any other type may
    // declare one, and the declaration makes it move-only.
    const u32 index = bag.emit<i18n::Key::AnalyzerViewCannotHaveDestructor>(
        DiagCode::DropOnCopy, name.span);
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
    needs_drop_.resize(type.idx + 1, 0);
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
  const ir::TypeNode node = builder.types()[type];
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
    case ir::TypeTag::Tuple: {
      const ir::TupleType tuple = builder.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : tuple.elements) {
        if (drop_scan(element, stack)) {
          result = true;
          break;
        }
      }
      break;
    }
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

// The nominal shape a type node names: two nodes over one struct or
// enum are the same declaration however they were interned.
std::optional<u64> Checker::drop_shape_key(ir::TypeIdx type) const {
  const ir::TypeNode node = builder.types()[type];
  if (node.tag == ir::TypeTag::Struct) {
    return (static_cast<u64>(ir::TypeTag::Struct) << 32) | node.as_struct().idx;
  }
  if (node.tag == ir::TypeTag::Enum) {
    return (static_cast<u64>(ir::TypeTag::Enum) << 32) | node.as_enum().idx;
  }
  return std::nullopt;
}

CheckedModule::DropGlue Checker::find_drop_glue(ir::TypeIdx type) {
  const auto found = drop_glue_by_type_.find(type.idx);
  if (found != drop_glue_by_type_.end()) {
    if (const std::optional<u64> shape = drop_shape_key(type);
        shape.has_value()) {
      drop_glue_by_shape_.emplace(*shape, found->second);
    }
    return found->second;
  }
  // A second type node over one nominal shape is the same declaration,
  // so the destructor the shape already answered with answers for it
  // too: a node interned through another path carries no glue of its
  // own, and a value of either ends the same way.
  const std::optional<u64> shape = drop_shape_key(type);
  if (shape.has_value()) {
    const auto shaped = drop_glue_by_shape_.find(*shape);
    if (shaped != drop_glue_by_shape_.end()) {
      return shaped->second;
    }
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
  const CheckedModule::DropGlue glue{position->second.first,
                                     position->second.second};
  if (shape.has_value()) {
    drop_glue_by_shape_.emplace(*shape, glue);
  }
  return glue;
}

bool Checker::holds_destructible(ir::TypeIdx type, std::vector<u32>& stack) {
  const ir::TypeNode node = builder.types()[type];
  if (node.tag == ir::TypeTag::Struct) {
    // The walk below builds glue, which can move the table this shape
    // was read from, so the shape is a copy and not a reference.
    const ir::StructType shape = builder.struct_types()[node.as_struct()];
    for (ir::TypeIdx field : shape.fields) {
      if (drop_scan(field, stack)) {
        return true;
      }
    }
    return false;
  }
  const ir::EnumType shape = builder.enum_types()[node.as_enum()];
  for (ir::EnumVariantTypeIdx v : shape.variants) {
    const ir::EnumVariantType variant = builder.enum_variant_types()[v];
    for (ir::TypeIdx payload : variant.fields) {
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
      const ir::StructType struct_type =
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
        const ir::EnumVariantType variant = storage.enum_variant_types()[vidx];
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
      const ir::TupleType tuple = storage.tuple_types()[node.as_tuple()];
      for (ir::TypeIdx element : tuple.elements) {
        if (has_value_cycle(element, stack, storage)) {
          cycle = true;
          break;
        }
      }
      break;
    }
    case ir::TypeTag::Array: {
      const ir::ArrayType array = storage.array_types()[node.as_array()];
      cycle = has_value_cycle(array.element, stack, storage);
      break;
    }
    case ir::TypeTag::Slice: {
      const ir::SliceType slice = storage.slice_types()[node.as_slice()];
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

// Whether the backend compares the type directly. A nominal type or
// `str` compares through `PartialEq` (ADR-0053); a tuple or array does
// so when each element can be compared directly, because the builtin
// comparison is element-wise.
bool Checker::is_builtin_comparable(ir::TypeIdx type) const {
  const ir::TypeTag tag = tag_of(type);
  if (is_integer_tag(tag) || is_float_tag(tag) || tag == ir::TypeTag::I1 ||
      tag == ir::TypeTag::Ptr || tag == ir::TypeTag::Ref ||
      tag == ir::TypeTag::MutRef || ir::is_raw_ptr_type(tag)) {
    return true;
  }
  if (tag == ir::TypeTag::Tuple) {
    const ir::TupleType tuple =
        builder.tuple_types()[builder.types()[type].as_tuple()];
    for (const ir::TypeIdx element : tuple.elements) {
      if (!is_builtin_comparable(element)) {
        return false;
      }
    }
    return true;
  }
  if (tag == ir::TypeTag::Array) {
    const ir::ArrayType array =
        builder.array_types()[builder.types()[type].as_array()];
    return is_builtin_comparable(array.element);
  }
  return false;
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
      const ir::ArrayType aa =
          builder.array_types()[builder.types()[a].as_array()];
      const ir::ArrayType ab =
          builder.array_types()[builder.types()[b].as_array()];
      return aa.count == ab.count &&
             types_equal_inner(aa.element, ab.element, seen);
    }
    case ir::TypeTag::Slice: {
      const ir::SliceType sa =
          builder.slice_types()[builder.types()[a].as_slice()];
      const ir::SliceType sb =
          builder.slice_types()[builder.types()[b].as_slice()];
      return types_equal_inner(sa.element, sb.element, seen);
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType ta_t =
          builder.tuple_types()[builder.types()[a].as_tuple()];
      const ir::TupleType tb_t =
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
      const ir::FuncType fa =
          builder.func_types()[builder.types()[a].as_func()];
      const ir::FuncType fb =
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
    case ir::TypeTag::Const: {
      // Equal tags do not make equal arguments: the value is the type.
      const u64 left =
          builder.const_types()[builder.types()[a].as_const()].value;
      const u64 right =
          builder.const_types()[builder.types()[b].as_const()].value;
      return left == right;
    }
    case ir::TypeTag::Param: {
      // The index is the identity: one node serves every item's
      // position with that index, and each item's own scope says what
      // the position means (ADR-0079).
      return builder.types()[a].as_param().value ==
             builder.types()[b].as_param().value;
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
  const ir::SliceType slice =
      builder.slice_types()[builder.types()[epointee].as_slice()];
  const ir::ArrayType array =
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
  // A closure value coerces to the function type its environment fits:
  // the view borrows the environment the value owns (ADR-0064).
  if (const ClosureSig* closure = closure_sig_of_type(actual);
      closure != nullptr && tag_of(expected) == ir::TypeTag::Func) {
    const ir::FuncType shape =
        builder.func_types()[builder.types()[expected].as_func()];
    bool matches = shape.params.size() == closure->params.size() &&
                   types_equal(shape.ret, closure->ret);
    for (usize i = 0; matches && i < closure->params.size(); ++i) {
      matches = types_equal(shape.params[i], closure->params[i]);
    }
    if (matches) {
      return expected;
    }
  }
  // An unwritten slot is the one mismatch with an obvious fix, so it
  // gets its own message instead of two type names.
  const ir::TypeIdx uninit_side = uninit_payload(expected).is_valid() ? expected
                                  : uninit_payload(actual).is_valid()
                                      ? actual
                                      : ir::TypeIdx::invalid();
  if (uninit_side.is_valid()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerUninitializedValueUsed>(
        DiagCode::TypeMismatch, span,
        pretty_tag(tag_of(uninit_side == expected ? actual : expected)));
    (void)index;
    return error_type();
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerTypeMismatchExpectedFound>(
      DiagCode::TypeMismatch, span, what, type_display(expected),
      type_display(actual));
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

ir::TypeIdx Checker::primitive_by_name(std::string_view name) {
  struct Spell {
    std::string_view text;
    ir::TypeTag tag;
  };
  constexpr Spell SPELLS[] = {
      {"i8", ir::TypeTag::I8},   {"i16", ir::TypeTag::I16},
      {"i32", ir::TypeTag::I32}, {"i64", ir::TypeTag::I64},
      {"u8", ir::TypeTag::U8},   {"u16", ir::TypeTag::U16},
      {"u32", ir::TypeTag::U32}, {"u64", ir::TypeTag::U64},
      {"f32", ir::TypeTag::F32}, {"f64", ir::TypeTag::F64},
  };
  for (const Spell& spell : SPELLS) {
    if (name == spell.text) {
      return builder.primitive(spell.tag);
    }
  }
  // The pointer-width spellings name the target's width (ADR-0121).
  if (name == "isize") {
    return builder.primitive(width == ir::PointerWidth::W64 ? ir::TypeTag::I64
                                                            : ir::TypeTag::I32);
  }
  if (name == "usize") {
    return builder.primitive(width == ir::PointerWidth::W64 ? ir::TypeTag::U64
                                                            : ir::TypeTag::U32);
  }
  return error_type();
}

ir::TypeIdx Checker::head_named_type(std::string_view name) {
  const ir::TypeIdx builtin = primitive_by_name(name);
  if (!is_error(builtin)) {
    return builtin;
  }
  for (usize i = type_params.size(); i > 0; --i) {
    if (type_params[i - 1].first == name) {
      return type_params[i - 1].second;
    }
  }
  return error_type();
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
      DiagCode::UnsupportedType, span, suffix);
  (void)index;
  tag = TT::Error;
  return true;
}

// Whether an integer literal's magnitude fits the type it was given.
// A signed minimum is one past the positive maximum, so a negation has
// room the positive form does not.
bool Checker::int_literal_fits(std::string_view spelling,
                               ir::TypeTag tag,
                               bool negative) {
  bool overflow = false;
  const u64 magnitude = comp::parse_numeric_value(spelling, &overflow);
  // Past u64 is past every type, and the parse wraps there.
  if (overflow) {
    return false;
  }
  switch (tag) {
    case ir::TypeTag::U8: return magnitude <= (negative ? 0ull : 0xFFull);
    case ir::TypeTag::U16: return magnitude <= (negative ? 0ull : 0xFFFFull);
    case ir::TypeTag::U32:
      return magnitude <= (negative ? 0ull : 0xFFFFFFFFull);
    case ir::TypeTag::U64: return !negative || magnitude == 0;
    case ir::TypeTag::I8: return magnitude <= (negative ? 0x80ull : 0x7Full);
    case ir::TypeTag::I16:
      return magnitude <= (negative ? 0x8000ull : 0x7FFFull);
    case ir::TypeTag::I32:
      return magnitude <= (negative ? 0x80000000ull : 0x7FFFFFFFull);
    case ir::TypeTag::I64:
      return magnitude <=
             (negative ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull);
    default: return true;
  }
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
            DiagCode::TypeMismatch, lit.span);
        (void)index;
        return error_type();
      }
      const bool negative = lit.is_negative != literal_negated_;
      if (!has_suffix && expected != nullptr &&
          tag_of(*expected) == ir::TypeTag::Param) {
        // The parameter stands for an integer type the instantiation
        // chooses: the definition-site check adopts it, and the width
        // settles where the argument is known (ADR-0079).
        return *expected;
      }
      if (!has_suffix && expected != nullptr &&
          is_integer_tag(tag_of(*expected))) {
        const ir::TypeTag wanted = tag_of(*expected);
        if (!int_literal_fits(lit.spelling, wanted, negative)) {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerIntegerLiteralOutOfRange>(
                  DiagCode::TypeMismatch, lit.span);
          (void)index;
          return error_type();
        }
        return *expected;
      }
      // The literal takes its suffix's type, or the default. The
      // magnitude must fit either: without the check a value past the
      // type truncated silently to a different number.
      if (!int_literal_fits(lit.spelling, tag, negative)) {
        const u32 index = bag.emit<i18n::Key::AnalyzerIntegerLiteralOutOfRange>(
            DiagCode::TypeMismatch, lit.span);
        (void)index;
        return error_type();
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
            DiagCode::TypeMismatch, lit.span);
        (void)index;
        return error_type();
      }
      if (!has_suffix && expected != nullptr &&
          (is_float_tag(tag_of(*expected)) ||
           tag_of(*expected) == ir::TypeTag::Param)) {
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
          DiagCode::RecursiveType, entry.span, entry.name);
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
          DiagCode::RecursiveType, nominals[instance.nominal].span,
          nominals[instance.nominal].name);
      (void)index;
    }
  }
}

// Value namespace

bool Checker::const_array_length(u32 module,
                                 ast::PathIdx path,
                                 diag::Span span,
                                 u64& out) {
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.empty()) {
    return false;
  }
  const std::string_view name = segments.back().name;
  if (segments.size() == 1) {
    // A value parameter of the enclosing declaration shadows an item of
    // the same name, exactly as a type parameter does.
    if (const ValueParam* bound = value_param(name)) {
      out = bound->value;
      return true;
    }
    // A type parameter names a type, never a length: consult it before
    // the item namespaces so an outer item cannot answer for it.
    for (usize i = type_params.size(); i > 0; --i) {
      if (type_params[i - 1].first == name) {
        const u32 index = bag.emit<i18n::Key::AnalyzerTypeParameterAsValue>(
            DiagCode::UnknownType, span, name);
        (void)index;
        return false;
      }
    }
  }
  u32 target = NO_MODULE;
  if (!walk_module_prefix(module, path, "value", target)) {
    return false;
  }
  const CheckedModule::StaticInfo* info = lookup_static(target, name);
  if (info != nullptr &&
      !check_item_access(module, info->module, info->is_pub, span, name)) {
    return false;
  }
  if (info == nullptr || !info->is_const || !info->init.is_valid()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerArrayLengthNotConst>(
        DiagCode::InvalidOperation, span);
    (void)index;
    return false;
  }
  // The initializer evaluates in the module that declares the item,
  // and registering a dependency can grow the table this pointer names,
  // so carry a copy.
  const CheckedModule::StaticInfo definition = *info;
  return const_int_value<i18n::Key::AnalyzerArrayLengthNotConst>(definition,
                                                                 span, out);
}

template <i18n::Key not_const_key>
bool Checker::const_int_value(const CheckedModule::StaticInfo& info,
                              diag::Span span,
                              u64& out) {
  const auto cached = const_int_values_.find(info.init.idx);
  if (cached != const_int_values_.end()) {
    out = cached->second;
    return true;
  }
  const auto wrong = [&] {
    const u32 index = bag.emit<not_const_key>(DiagCode::InvalidOperation, span);
    (void)index;
    return false;
  };
  const ir::TypeTag tag = tag_of(info.type);
  if (!is_integer_tag(tag)) {
    return wrong();
  }
  // The check diagnostics speak for themselves; a value that fails here
  // after checking is the evaluator's refusal.
  if (!ensure_const_checked(info)) {
    return false;
  }
  comp::CompVal value;
  CompEvaluator eval(CompInputs{tree, modules, {}, type_origins_, fn_instances},
                     ast, builder, width);
  eval.set_call_recorder(this, &Checker::record_comp_call);
  if (!eval.evaluate_item(info.module, info.init, cur_inst, value) ||
      value.value.tag != comp::CompValue::Tag::Int) {
    return wrong();
  }
  // A length is a count in the declared type's range, so a literal
  // wider than the tag narrows exactly as lowering will.
  const u64 narrowed = value.value.int_value & comp::mask(tag);
  // A signed const that evaluates below zero has no length.
  if (comp::is_signed(tag) && comp::sign_extend(narrowed, tag) < 0) {
    return wrong();
  }
  out = narrowed;
  const_int_values_.emplace(info.init.idx, out);
  return true;
}

bool Checker::ensure_const_checked(const CheckedModule::StaticInfo& info) {
  const u32 key = info.init.idx;
  if (const_init_checked_.contains(key)) {
    return !const_init_failed_.contains(key);
  }
  if (const_init_in_progress_.contains(key)) {
    const u32 index = bag.emit<i18n::Key::AnalyzerConstCycle>(
        DiagCode::InvalidOperation, ast.exprs[info.init].span, info.name);
    (void)index;
    return false;
  }
  const_init_in_progress_.insert(key);
  bool ok = check_const_dependencies(info.module, info.init);
  if (ok && !const_init_checked_.contains(key)) {
    const ir::TypeIdx want = info.type;
    const bool saved_in_fn = in_fn;
    in_fn = false;
    scopes.emplace_back();
    ++comp_depth;
    const ir::TypeIdx actual = check_expr(info.module, info.init, &want);
    --comp_depth;
    scopes.pop_back();
    in_fn = saved_in_fn;
    ok = !is_error(actual);
    if (!ok) {
      const_init_failed_.insert(key);
    }
    const_init_checked_.insert(key);
  }
  const_init_in_progress_.erase(key);
  return ok;
}

bool Checker::check_const_dependencies(u32 module, ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Literal: return true;
    case ast::ExprKind::Path:
      return check_const_dependency_path(module,
                                         node.payload.get<ast::ExprPath>().idx);
    case ast::ExprKind::Unary:
      return check_const_dependencies(module,
                                      node.payload.get<ast::ExprUnary>().inner);
    case ast::ExprKind::Borrow:
      return check_const_dependencies(
          module, node.payload.get<ast::ExprBorrow>().inner);
    case ast::ExprKind::Deref:
      return check_const_dependencies(module,
                                      node.payload.get<ast::ExprDeref>().inner);
    case ast::ExprKind::Binary: {
      const ast::ExprBinary& binary = node.payload.get<ast::ExprBinary>();
      return check_const_dependencies(module, binary.lhs) &&
             check_const_dependencies(module, binary.rhs);
    }
    case ast::ExprKind::Cast:
      return check_const_dependencies(module,
                                      node.payload.get<ast::ExprCast>().inner);
    case ast::ExprKind::Tuple: {
      for (ast::ExprIdx element : node.payload.get<ast::ExprTuple>().elements) {
        if (!check_const_dependencies(module, element)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::Array: {
      const ast::ExprArray& array = node.payload.get<ast::ExprArray>();
      if (array.repeat.is_valid()) {
        return check_const_dependencies(module, array.repeat);
      }
      for (ast::ExprIdx element : array.elements) {
        if (!check_const_dependencies(module, element)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::Struct: {
      const ast::ExprStruct& structure = node.payload.get<ast::ExprStruct>();
      for (const ast::ExprFieldInit& field : structure.init) {
        if (!check_const_dependencies(module, field.value)) {
          return false;
        }
      }
      return !structure.base_expr.is_valid() ||
             check_const_dependencies(module, structure.base_expr);
    }
    case ast::ExprKind::Field:
      return check_const_dependencies(
          module, node.payload.get<ast::ExprField>().receiver);
    case ast::ExprKind::Index: {
      const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
      return check_const_dependencies(module, index.receiver) &&
             check_const_dependencies(module, index.index);
    }
    case ast::ExprKind::Call: {
      const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
      precheck_called_fn(module, call.callee);
      for (ast::ExprIdx arg : call.args) {
        if (!check_const_dependencies(module, arg)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::MethodCall: {
      const ast::ExprMethodCall& call = node.payload.get<ast::ExprMethodCall>();
      if (!check_const_dependencies(module, call.receiver)) {
        return false;
      }
      for (ast::ExprIdx arg : call.args) {
        if (!check_const_dependencies(module, arg)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::Question:
      return check_const_dependencies(
          module, node.payload.get<ast::ExprQuestion>().inner);
    case ast::ExprKind::If: {
      const ast::ExprIf& if_node = node.payload.get<ast::ExprIf>();
      const ast::Cond& cond = ast.conds[if_node.cond];
      if (!check_const_dependencies(module, cond.value) ||
          !check_const_dependencies_block(module, if_node.then_block)) {
        return false;
      }
      return !if_node.else_block.is_valid() ||
             check_const_dependencies_block(module, if_node.else_block);
    }
    case ast::ExprKind::Match: {
      const ast::ExprMatch& match = node.payload.get<ast::ExprMatch>();
      if (!check_const_dependencies(module, match.scrutinee)) {
        return false;
      }
      for (const ast::ExprMatchArm& arm : match.arms) {
        if (!check_const_dependencies(module, arm.body)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::Block:
      return check_const_dependencies_block(
          module, node.payload.get<ast::ExprBlock>().block);
    case ast::ExprKind::Loop:
      return check_const_dependencies_block(
          module, node.payload.get<ast::ExprLoop>().body);
    case ast::ExprKind::While: {
      const ast::ExprWhile& while_node = node.payload.get<ast::ExprWhile>();
      const ast::Cond& cond = ast.conds[while_node.cond];
      if (!check_const_dependencies(module, cond.value)) {
        return false;
      }
      return check_const_dependencies_block(module, while_node.body);
    }
    case ast::ExprKind::Break:
    case ast::ExprKind::Continue:
    case ast::ExprKind::Return:
    case ast::ExprKind::Range:
    case ast::ExprKind::Closure: return true;
  }
}

bool Checker::check_const_dependencies_block(u32 module, ast::BlockIdx block) {
  const ast::Block& body = ast.blocks[block];
  for (ast::StmtIdx stmt : body.statements) {
    const ast::StmtNode& node = ast.stmts[stmt];
    switch (node.kind) {
      case ast::StmtKind::Decl:
        if (!check_const_dependencies(module,
                                      node.payload.get<ast::StmtDecl>().init)) {
          return false;
        }
        continue;
      case ast::StmtKind::Reassign: {
        const ast::StmtReassign& reassign =
            node.payload.get<ast::StmtReassign>();
        if (!check_const_dependencies(module, reassign.place) ||
            !check_const_dependencies(module, reassign.value)) {
          return false;
        }
        continue;
      }
      case ast::StmtKind::Expr:
        if (!check_const_dependencies(
                module, node.payload.get<ast::StmtExpr>().value)) {
          return false;
        }
        continue;
    }
  }
  return !body.value.is_valid() || check_const_dependencies(module, body.value);
}

bool Checker::check_const_dependency_path(u32 module, ast::PathIdx path) {
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() != 1) {
    return true;
  }
  const CheckedModule::StaticInfo* info =
      lookup_static(module, segments[0].name);
  if (info == nullptr || !info->is_const || !info->init.is_valid()) {
    return true;
  }
  const CheckedModule::StaticInfo definition = *info;
  return ensure_const_checked(definition);
}

void Checker::precheck_called_fn(u32 module, ast::ExprIdx callee) {
  if (ast.exprs[callee].kind != ast::ExprKind::Path) {
    return;
  }
  const ast::PathIdx path = ast.exprs[callee].payload.get<ast::ExprPath>().idx;
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() != 1) {
    return;
  }
  const CheckedModule::FnSig* fn = lookup_function(module, segments[0].name);
  if (fn == nullptr) {
    return;
  }
  precheck_fn_body(*fn);
}

void Checker::precheck_fn_body(const CheckedModule::FnSig& fn) {
  if (!fn.item.is_valid() || prechecked_fns_.contains(fn.item.idx)) {
    return;
  }
  const ast::ItemNode& item = ast.items[fn.item];
  if (item.kind != ast::ItemKind::Fn ||
      !item.payload.get<ast::ItemFn>().generic.empty()) {
    return;
  }
  const auto where = position_by_address_.find(&fn);
  if (where == position_by_address_.end()) {
    return;
  }
  const u32 module = where->second.first;
  const u32 inst = fn.inst;
  prechecked_fns_.insert(fn.item.idx);
  std::vector<std::pair<std::string_view, ir::TypeIdx>> saved =
      std::move(type_params);
  std::vector<ValueParam> saved_values = std::move(value_params);
  type_params.clear();
  value_params.clear();
  // check_fn resets the per-body state and leaves it reset; the
  // precheck runs in the middle of whatever body asked for the value,
  // so every field it writes is saved and restored. The callee keys its
  // expressions under its own instantiation, the way the evaluator
  // reads them.
  const ir::TypeIdx saved_ret = fn_ret;
  const u32 saved_loop = loop_depth;
  const bool saved_in_fn = in_fn;
  const bool saved_bind = bind_comp_known;
  const bool saved_too_deep = reported_too_deep_;
  const u32 saved_inst = cur_inst;
  const u32 saved_comp = comp_depth;
  cur_inst = inst;
  // The callee is an ordinary function body wherever the precheck was
  // reached from; only its typed expressions are wanted, so the comp
  // rules of the caller do not apply to it.
  comp_depth = 0;
  ++prechecking_fn_;
  check_fn(module, fn.item, nullptr);
  --prechecking_fn_;
  comp_depth = saved_comp;
  cur_inst = saved_inst;
  reported_too_deep_ = saved_too_deep;
  bind_comp_known = saved_bind;
  fn_ret = saved_ret;
  loop_depth = saved_loop;
  in_fn = saved_in_fn;
  type_params = std::move(saved);
  value_params = std::move(saved_values);
}

void Checker::precheck_called_method(const CheckedModule::MethodInfo& method) {
  if (!method.item.is_valid() || prechecked_fns_.contains(method.item.idx)) {
    return;
  }
  // A method of a generic impl was checked under its instantiation the
  // first time a call resolved it, so only a concrete impl waits for
  // the bodies sweep.
  if (generic_find(method.self_type) != nullptr) {
    return;
  }
  const ast::ItemNode& node = ast.items[method.item];
  if (node.kind != ast::ItemKind::Fn ||
      !node.payload.get<ast::ItemFn>().generic.empty()) {
    return;
  }
  const auto where = position_by_address_.find(&method);
  if (where == position_by_address_.end()) {
    return;
  }
  const u32 module = where->second.first;
  prechecked_fns_.insert(method.item.idx);
  const u32 inst = inst_index(method.self_type);
  const ir::TypeIdx saved_ret = fn_ret;
  const u32 saved_loop = loop_depth;
  const bool saved_in_fn = in_fn;
  const bool saved_bind = bind_comp_known;
  const bool saved_too_deep = reported_too_deep_;
  const u32 saved_inst = cur_inst;
  const u32 saved_comp = comp_depth;
  const usize saved_specs = spec_scope.size();
  cur_inst = inst;
  comp_depth = 0;
  if (method.spec != NO_SPEC) {
    spec_scope.push_back(method.spec);
  }
  ++prechecking_fn_;
  check_fn(module, method.item, &method.self_type);
  --prechecking_fn_;
  spec_scope.resize(saved_specs);
  comp_depth = saved_comp;
  cur_inst = saved_inst;
  reported_too_deep_ = saved_too_deep;
  bind_comp_known = saved_bind;
  fn_ret = saved_ret;
  loop_depth = saved_loop;
  in_fn = saved_in_fn;
}

bool Checker::ensure_function_named(u32 module, std::string_view name) {
  for (ast::ItemIdx item : tree.modules[module]->items) {
    const ast::ItemNode& node = ast.items[item];
    if (node.kind != ast::ItemKind::Fn) {
      continue;
    }
    const ast::ItemFn& fn = node.payload.get<ast::ItemFn>();
    if (fn.name.name != name || !fn.generic.empty()) {
      continue;
    }
    register_fn_signature(module, item);
    return true;
  }
  return false;
}

bool Checker::ensure_static_named(u32 module, std::string_view name) {
  for (ast::ItemIdx item : tree.modules[module]->items) {
    const ast::ItemNode& node = ast.items[item];
    std::string_view declared;
    if (node.kind == ast::ItemKind::Const) {
      declared = node.payload.get<ast::ItemConst>().name.name;
    } else if (node.kind == ast::ItemKind::Static) {
      declared = node.payload.get<ast::ItemStatic>().name.name;
    } else {
      continue;
    }
    if (declared != name) {
      continue;
    }
    register_static(module, item);
    return true;
  }
  return false;
}

void Checker::register_fn_signature(u32 module, ast::ItemIdx item) {
  const ast::ItemFn& fn = ast.items[item].payload.get<ast::ItemFn>();
  if (!fn.generic.empty() || !registered_items_.insert(item.idx).second) {
    return;
  }
  std::vector<ir::TypeIdx> params;
  bool is_variadic = false;
  for (const ast::ItemFnParam& param : fn.params) {
    if (param.is_variadic) {
      is_variadic = true;
      continue;
    }
    params.push_back(resolve_type(module, param.type, nullptr));
  }
  ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
  if (fn.return_type.is_valid()) {
    ret = resolve_type(module, fn.return_type, nullptr);
  }
  add_function(module, {.name = fn.name.name,
                        .params = std::move(params),
                        .ret = ret,
                        .item = item,
                        .is_unsafe = fn.is_unsafe,
                        .is_comp = fn.is_comp,
                        .is_variadic = is_variadic});
}

void Checker::register_static(u32 module, ast::ItemIdx item) {
  if (!registered_items_.insert(item.idx).second) {
    return;
  }
  const ast::ItemNode& node = ast.items[item];
  const bool is_const = node.kind == ast::ItemKind::Const;
  const ast::Ident name = is_const ? node.payload.get<ast::ItemConst>().name
                                   : node.payload.get<ast::ItemStatic>().name;
  const ast::TypeIdx type = is_const ? node.payload.get<ast::ItemConst>().type
                                     : node.payload.get<ast::ItemStatic>().type;
  const ast::ExprIdx init = is_const ? node.payload.get<ast::ItemConst>().init
                                     : node.payload.get<ast::ItemStatic>().init;
  modules[module].statics.push_back(
      {.name = name.name,
       .item = item,
       .type = resolve_type(module, type, nullptr),
       .init = init,
       .is_const = is_const,
       .module = module,
       .is_pub = node.is_pub});
}

const CheckedModule::StaticInfo* find_static(
    std::span<const CheckedModule> modules,
    const ModuleTree& tree,
    u32 module,
    std::string_view name) {
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

const CheckedModule::StaticInfo* Checker::lookup_static(u32 module,
                                                        std::string_view name) {
  if (const CheckedModule::StaticInfo* info =
          analyzer::find_static(modules, tree, module, name)) {
    return info;
  }
  ensure_static_named(module, name);
  if (const CheckedModule::StaticInfo* info =
          analyzer::find_static(modules, tree, module, name)) {
    return info;
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Value || import.name != name) {
      continue;
    }
    ensure_static_named(import.target_module, import.member);
  }
  return analyzer::find_static(modules, tree, module, name);
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

const CheckedModule::FnSig* Checker::lookup_function(u32 module,
                                                     std::string_view name) {
  // A generic function's registered signatures are per instantiation;
  // the name still resolves through lookup_generic_fn so each call
  // rebinds its parameters. A method's signature shares the table but
  // not the name: a bare call cannot reach it.
  const auto find =
      [this](u32 in_module,
             std::string_view wanted) -> const CheckedModule::FnSig* {
    for (const CheckedModule::FnSig& fn : modules[in_module].functions) {
      if (fn.name == wanted && !fn.is_method &&
          fn_generic_params(fn.item).empty()) {
        return &fn;
      }
    }
    return nullptr;
  };
  if (const CheckedModule::FnSig* fn = find(module, name)) {
    return fn;
  }
  // A use may outrun the signature sweep, as an array length can reach
  // a const whose initializer calls a function declared later; register
  // the declaration on demand so the order does not matter.
  ensure_function_named(module, name);
  if (const CheckedModule::FnSig* fn = find(module, name)) {
    return fn;
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Value || import.name != name) {
      continue;
    }
    if (const CheckedModule::FnSig* fn =
            find(import.target_module, import.member)) {
      return fn;
    }
    ensure_function_named(import.target_module, import.member);
    if (const CheckedModule::FnSig* fn =
            find(import.target_module, import.member)) {
      return fn;
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
        DiagCode::UnknownValue, span, name);
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

NominalEntry* Checker::find_impl_target(u32 module,
                                        ast::TypeIdx type,
                                        std::string_view& name,
                                        bool* resolved) {
  const ast::TypeNode& node = ast.types[type];
  if (node.kind != ast::TypeKind::Path) {
    return nullptr;
  }
  const std::span<const ast::Ident> segments =
      ast.paths[node.payload.get<ast::TypePath>().path].segments;
  if (segments.empty()) {
    return nullptr;
  }
  name = segments.back().name;
  if (segments.size() == 1) {
    if (resolved != nullptr) {
      *resolved = true;
    }
    return find_nominal_in_scope(module, name);
  }
  u32 target_module = NO_MODULE;
  std::string_view target_name;
  if (!resolve_type_path(module, node.payload.get<ast::TypePath>().path,
                         target_module, target_name)) {
    return nullptr;
  }
  if (resolved != nullptr) {
    *resolved = true;
  }
  name = target_name;
  return find_nominal(target_module, target_name);
}

bool Checker::check_item_access(u32 from,
                                u32 declaring,
                                bool is_pub,
                                diag::Span span,
                                std::string_view name) {
  if (declaration_visible(from, declaring, is_pub)) {
    return true;
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerPrivateItem>(
      DiagCode::PrivateAccess, span, name);
  (void)index;
  return false;
}

bool Checker::check_field_access(u32 from,
                                 u32 declaring,
                                 bool is_pub,
                                 diag::Span span,
                                 std::string_view field,
                                 std::string_view owner) {
  if (declaration_visible(from, declaring, is_pub)) {
    return true;
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerPrivateField>(
      DiagCode::PrivateAccess, span, field, owner);
  (void)index;
  return false;
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
      // settled where the impl was written.
      if (impl.spec.is_valid() && !impl.params.empty()) {
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

const CheckedModule::MethodInfo* Checker::instantiate_generic_method(
    u32 module,
    ir::TypeIdx self,
    std::string_view name,
    std::span<const ast::TypeIdx> type_args,
    std::span<const ast::ExprIdx> args,
    diag::Span span) {
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
  if (!has_nominal) {
    return nullptr;
  }
  if (!generic_methods_indexed_ || indexed_nominals_ != nominals.size()) {
    index_generic_methods();
  }
  if (target_nominal >= generic_methods_by_nominal_.size()) {
    return nullptr;
  }
  for (const GenericMethodEntry& entry :
       generic_methods_by_nominal_[target_nominal]) {
    const ast::ItemImpl& impl =
        ast.items[entry.impl_item].payload.get<ast::ItemImpl>();
    const ast::ItemFn& method_fn =
        ast.items[entry.method_item].payload.get<ast::ItemFn>();
    if (method_fn.name.name != name) {
      continue;
    }
    if (!check_item_access(module, entry.module,
                           ast.items[entry.method_item].is_pub, span, name)) {
      return nullptr;
    }
    // The impl's parameters bind from the instance's arguments: a
    // direct name binds, and a concrete argument must name the same
    // type the instance carries.
    const std::span<const ast::TypeIdx> target_nodes =
        ast.types[impl.type].payload.get<ast::TypePath>().args;
    if (target_nodes.size() != target_args.size()) {
      continue;
    }
    std::vector<std::pair<std::string_view, ir::TypeIdx>> scope;
    std::vector<ValueParam> value_scope;
    bool shape_ok = true;
    for (usize i = 0; i < target_nodes.size() && shape_ok; ++i) {
      const ast::TypeNode& arg_node = ast.types[target_nodes[i]];
      bool direct = false;
      if (arg_node.kind == ast::TypeKind::Path) {
        const ast::TypePath& arg_path = arg_node.payload.get<ast::TypePath>();
        const ast::Path& path = ast.paths[arg_path.path];
        if (path.segments.size() == 1 && arg_path.args.empty()) {
          for (const ast::GenericParam& param : impl.params) {
            if (param.name.name != path.segments[0].name) {
              continue;
            }
            if (is_value_param(ast, param)) {
              if (tag_of(target_args[i]) != ir::TypeTag::Const) {
                shape_ok = false;
                break;
              }
              value_scope.push_back(ValueParam{param.name.name,
                                               const_type_value(target_args[i]),
                                               entry.module, param.type});
            } else {
              scope.emplace_back(param.name.name, target_args[i]);
            }
            direct = true;
            break;
          }
        }
      }
      if (direct) {
        continue;
      }
      const ir::TypeIdx declared =
          resolve_type(entry.module, target_nodes[i], nullptr);
      if (is_error(declared) || !types_equal(declared, target_args[i])) {
        shape_ok = false;
      }
    }
    if (!shape_ok) {
      continue;
    }
    // A method's own parameters infer from the call's arguments the
    // way a free function's do: the declaration pins what it can and
    // the closure signature fills a return slot. The receiver is the
    // call's receiver rather than a listed argument, so its slot
    // binds nothing here; a value that stays unbound waits for a
    // spelling the method path does not have.
    std::vector<usize> return_slots;
    std::vector<ir::TypeIdx> method_args;
    {
      // A turbofish spells only the parameters the declaration writes
      // (ADR-0075): a synthetic `fn(...)` slot takes its witness from
      // an argument or from the body, never from a spelling.
      usize spelled_count = 0;
      for (const ast::GenericParam& param : method_fn.generic) {
        if (own_fn_slot(param.name.name) == nullptr) {
          ++spelled_count;
        }
      }
      if (type_args.size() != spelled_count && !type_args.empty()) {
        const u32 index = bag.emit<i18n::Key::AnalyzerTypeArityMismatch>(
            DiagCode::ArityMismatch, span, name, spelled_count,
            spelled_count == 1 ? "" : "s");
        (void)index;
        return nullptr;
      }
      std::vector<ir::TypeIdx> inferred(method_fn.generic.size(),
                                        ir::TypeIdx(base::INVALID_IDX));
      usize next_explicit = 0;
      for (usize i = 0;
           i < method_fn.generic.size() && next_explicit < type_args.size();
           ++i) {
        if (own_fn_slot(method_fn.generic[i].name.name) != nullptr) {
          continue;
        }
        const ir::TypeIdx resolved =
            resolve_type(entry.module, type_args[next_explicit++], &self);
        if (is_error(resolved)) {
          return nullptr;
        }
        inferred[i] = resolved;
      }
      const std::span<const ast::ItemFnParam> declared =
          fn_params(entry.method_item);
      const usize pushed = type_params.size();
      for (const auto& binding : scope) {
        type_params.emplace_back(binding.first, binding.second);
      }
      // A receiver is written `self` (grammar.md), and the call's list
      // leaves it out, so the pairing starts after it. Reading the
      // spelling avoids resolving the first parameter's type, which
      // may name the very parameter being inferred.
      usize offset = 0;
      if (!declared.empty() &&
          (ast.patterns[declared[0].pattern].kind == ast::PatternKind::Ident ||
           ast.patterns[declared[0].pattern].kind ==
               ast::PatternKind::MutIdent)) {
        const std::string_view first_name =
            ast.patterns[declared[0].pattern].kind == ast::PatternKind::Ident
                ? ast.patterns[declared[0].pattern].payload.ident.name.name
                : ast.patterns[declared[0].pattern].payload.mut_ident.name.name;
        if (first_name == "self") {
          offset = 1;
        }
      }
      bool inference_ok = true;
      for (usize i = 0; i + offset < declared.size() && i < args.size(); ++i) {
        const DeclaredBinding slot_decl = declared_binding(
            method_fn.generic, ast.types[declared[i + offset].type]);
        if (slot_decl.slot >= method_fn.generic.size() ||
            is_value_param(ast, method_fn.generic[slot_decl.slot]) ||
            inferred[slot_decl.slot].is_valid()) {
          continue;
        }
        if (!bind_argument_slot(module, slot_decl, args[i], name, inferred,
                                span)) {
          inference_ok = false;
          break;
        }
      }
      for (usize i = 0;
           inference_ok && i + offset < declared.size() && i < args.size();
           ++i) {
        if (!bind_fn_slots(module, method_fn.generic, declared[i + offset].type,
                           args[i], inferred, span)) {
          inference_ok = false;
          break;
        }
      }
      if (inference_ok &&
          !infer_bound_arguments(entry.module, method_fn.generic, inferred,
                                 span)) {
        inference_ok = false;
      }
      while (type_params.size() > pushed) {
        type_params.pop_back();
      }
      if (!inference_ok) {
        return nullptr;
      }
      // A return-position spelling waits for the body's first `ret`,
      // the way a free function's does (ADR-0075); a witness an
      // earlier call recorded answers directly.
      for (usize i = 0; i < inferred.size(); ++i) {
        if (inferred[i].is_valid()) {
          continue;
        }
        const OwnFnSlot* own = own_fn_slot(method_fn.generic[i].name.name);
        if (own == nullptr || !own->is_return) {
          continue;
        }
        const ir::TypeIdx known = own_fn_return_witness(
            entry.method_item, i, witness_key(inferred, i));
        if (known.is_valid()) {
          inferred[i] = known;
          continue;
        }
        inferred[i] = own_fn_return_stand_in();
        return_slots.push_back(i);
      }
      bool complete = true;
      for (const ir::TypeIdx arg : inferred) {
        if (!arg.is_valid()) {
          complete = false;
        }
      }
      if (!complete) {
        const u32 index =
            bag.emit<i18n::Key::AnalyzerCannotInferMethodTypeArguments>(
                DiagCode::NotCompKnown, span, name, name);
        (void)index;
        return nullptr;
      }
      method_args = std::move(inferred);
    }
    // ADR-0085: a type argument is Movable, and a method instantiation
    // binds its own arguments the way a free function's does.
    if (!check_type_args_movable(method_args, span)) {
      return nullptr;
    }
    // The method's own bounds have to hold for the arguments it was
    // spelled with, the way a free function's do (ADR-0028).
    std::vector<std::pair<std::string_view, ir::TypeIdx>> method_bounds;
    for (usize i = 0; i < method_fn.generic.size() && i < method_args.size();
         ++i) {
      method_bounds.emplace_back(method_fn.generic[i].name.name,
                                 method_args[i]);
    }
    if (!check_declared_bounds(entry.module, method_fn.generic, method_bounds,
                               span)) {
      return nullptr;
    }
    // The body check that follows answers each fresh stand-in: the
    // instantiation consumes the record and takes the witness for the
    // return type, or reports that none came.
    for (usize slot : return_slots) {
      pending_own_fn_returns_.push_back(
          PendingOwnFnReturn{entry.method_item, slot, method_args[slot],
                             own_fn_slot(method_fn.generic[slot].name.name),
                             span, witness_key(method_args, slot)});
    }
    return instantiate_method(entry.module, self, scope, value_scope, impl,
                              name, method_args);
  }
  return nullptr;
}

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

const CheckedModule::MethodInfo* Checker::lookup_inherent_method(
    ir::TypeIdx self,
    std::string_view name,
    u32 module,
    diag::Span span) {
  const auto own = methods_by_self_.find(self.idx);
  if (own != methods_by_self_.end()) {
    for (const auto& [owner, position] : own->second) {
      const CheckedModule::MethodInfo& method =
          modules[owner].methods[position];
      if (method.spec != NO_SPEC) {
        continue;
      }
      // An entry keyed by spelled arguments is one call's
      // instantiation, not the impl's registration: a second call
      // with other arguments re-instantiates.
      if (!method.method_args.empty()) {
        continue;
      }
      if (method.name == name) {
        const bool is_pub =
            !method.item.is_valid() || ast.items[method.item].is_pub;
        (void)check_item_access(module, owner, is_pub, span, name);
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
      // Target arguments must name impl parameters directly. A value
      // parameter binds the const argument at its position; only a
      // const can carry the count the method's types resolve with.
      std::vector<std::pair<std::string_view, ir::TypeIdx>> scope;
      std::vector<ValueParam> value_scope;
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
        for (const ast::GenericParam& param : impl.params) {
          if (param.name.name != path.segments[0].name) {
            continue;
          }
          if (!is_value_param(ast, param)) {
            scope.emplace_back(param.name.name, args[i]);
            found = true;
            break;
          }
          if (tag_of(args[i]) == ir::TypeTag::Const) {
            value_scope.push_back(
                {param.name.name, const_type_value(args[i]), m, param.type});
            found = true;
          }
          break;
        }
        shape_ok = found;
      }
      if (!shape_ok) {
        continue;
      }
      // Only a block that declares the name is a candidate. An
      // unrelated block's bounds are not this call's to fail: checking
      // them first refused `Vec<f64>::iter` for `T: Ord`, which the
      // sorting block asks of a method it does not hold.
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
      // The impl's bounds have to hold for this instantiation before
      // its body specializes (ADR-0028): the refusal names the type
      // the caller passed rather than a line inside the library.
      if (!check_declared_bounds(m, impl.params, scope, span)) {
        continue;
      }
      const CheckedModule::MethodInfo* method =
          instantiate_method(m, self, scope, value_scope, impl, name);
      if (method != nullptr) {
        const bool is_pub =
            !method->item.is_valid() || ast.items[method->item].is_pub;
        (void)check_item_access(module, m, is_pub, span, name);
        return method;
      }
    }
  }
  // A compound target has no nominal to index by: match each recorded
  // block's shape against the receiver.
  for (const CompoundImplEntry& entry : compound_impls_) {
    const ast::ItemImpl& impl =
        ast.items[entry.item].payload.get<ast::ItemImpl>();
    std::vector<std::pair<std::string_view, ir::TypeIdx>> scope;
    std::vector<ValueParam> value_scope;
    if (!match_compound_target(entry.module, impl.type, impl.params, self,
                               scope, value_scope)) {
      continue;
    }
    const CheckedModule::MethodInfo* method =
        instantiate_method(entry.module, self, scope, value_scope, impl, name);
    if (method != nullptr) {
      const bool is_pub =
          !method->item.is_valid() || ast.items[method->item].is_pub;
      (void)check_item_access(module, entry.module, is_pub, span, name);
      return method;
    }
  }
  return nullptr;
}

// The method a type parameter's declared bounds provide (ADR-0080): a
// bound's spec declares it, its signature resolves with the bound's
// arguments and `Self` at the parameter, and the row it registers
// carries the parameter as its self type, so the call site answers
// with it the way it answers with an implementation's. The row's
// records key PARAM_INST: no lowering reads them.
const CheckedModule::MethodInfo* Checker::lookup_param_method(
    ir::TypeIdx param,
    std::string_view name,
    u32 module,
    diag::Span span,
    bool spec_only,
    u32 spec_filter,
    std::span<const ast::TypeIdx> method_type_args) {
  const ast::GenericParam* declared =
      param_at(builder.types()[param.idx].as_param().value);
  if (declared == nullptr) {
    return nullptr;
  }
  // An operator or a `for` head reports its own missing capability, so
  // those failures stay silent here; a plain method call names the
  // parameter and the method it asked for.
  const bool own_report = spec_filter == U32_MAX && !spec_only;
  const auto reject = [&]() {
    if (own_report) {
      const u32 index = bag.emit<i18n::Key::AnalyzerParamMethodMissing>(
          DiagCode::UnknownValue, span, declared->name.name, name);
      (void)index;
      // The caller reports the capability, not a second missing name.
      bound_failure_ = true;
    }
    return static_cast<const CheckedModule::MethodInfo*>(nullptr);
  };
  std::vector<DeclaredBound> bounds;
  declared_bounds(module, *declared, bounds);
  for (const DeclaredBound& bound : bounds) {
    const u32 spec = bound.spec;
    const ast::TypePath& path = bound.path;
    // The bound's spec provides the method, or one it refines does
    // (`Ord` answers through `PartialOrd`, ADR-0028): walk the chain
    // for the node that declares the name and meets the scope the call
    // asked for.
    u32 dispatch = U32_MAX;
    for (u32 node = spec; node != U32_MAX && node < spec_supers_.size();) {
      const bool accepted =
          spec_filter != U32_MAX
              ? (node == spec_filter || spec_refines(node, spec_filter))
              : spec_in_scope(module, specs[node]);
      if (accepted) {
        const ast::ItemSpec& candidate =
            ast.items[specs[node].item].payload.get<ast::ItemSpec>();
        for (const ast::SpecMethod& method : candidate.methods) {
          if (method.name.name == name) {
            dispatch = node;
            break;
          }
        }
      }
      if (dispatch != U32_MAX) {
        break;
      }
      node = spec_supers_[node];
    }
    if (dispatch == U32_MAX) {
      continue;
    }
    // The declared spec's arguments are the bound's own; a refined
    // node (a super) takes none, since a generic super is deferred.
    std::vector<ir::TypeIdx> spec_args;
    if (dispatch == spec) {
      for (ast::TypeIdx arg : path.args) {
        const ir::TypeIdx resolved = resolve_type(module, arg, nullptr);
        if (is_error(resolved)) {
          return reject();
        }
        spec_args.push_back(resolved);
      }
    }
    std::vector<ir::TypeIdx> method_args;
    for (ast::TypeIdx arg : method_type_args) {
      const ir::TypeIdx resolved = resolve_type(module, arg, nullptr);
      if (is_error(resolved)) {
        return reject();
      }
      method_args.push_back(resolved);
    }
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret = ir::TypeIdx::invalid();
    CheckedModule::ReceiverKind receiver = CheckedModule::ReceiverKind::None;
    if (!spec_method_sig(dispatch, name, spec_args, param, module, method_args,
                         params, ret, receiver)) {
      return reject();
    }
    if (receiver == CheckedModule::ReceiverKind::None) {
      return reject();
    }
    for (CheckedModule::MethodInfo& existing : modules[module].methods) {
      // The bound's arguments are part of the key: two parameters share
      // an index across items, and their bounds are what says which
      // call the row answers (ADR-0080).
      if (existing.spec == dispatch && existing.self_type.idx == param.idx &&
          existing.name == name &&
          std::ranges::equal(existing.method_args, method_args) &&
          std::ranges::equal(existing.params, params) &&
          types_equal(existing.ret, ret)) {
        return &existing;
      }
    }
    CheckedModule::MethodInfo& row =
        add_method(module, {
                               .self_type = param,
                               .name = name,
                               .params = params,
                               .ret = ret,
                               .receiver = receiver,
                               .item = ast::ItemIdx::invalid(),
                               .spec = dispatch,
                               .method_args = method_args,
                           });
    return &row;
  }
  return reject();
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
                                          scope, value_scope, impl, name);
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

bool Checker::fn_is_comp(ast::ItemIdx item) const {
  const ast::ItemNode& node = ast.items[item];
  return node.kind == ast::ItemKind::Fn &&
         node.payload.get<ast::ItemFn>().is_comp;
}

const Checker::OwnFnSlot* Checker::own_fn_slot(std::string_view name) const {
  for (const OwnFnSlot& slot : own_fn_slots_) {
    if (slot.name == name) {
      return &slot;
    }
  }
  return nullptr;
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

std::span<const ast::GenericParam> Checker::fn_generic_params(
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
static u32 param_slot(std::span<const ast::GenericParam> params,
                      std::string_view name) {
  for (u32 i = 0; i < static_cast<u32>(params.size()); ++i) {
    if (params[i].name.name == name) {
      return i;
    }
  }
  return static_cast<u32>(params.size());
}

Checker::DeclaredBinding Checker::declared_binding(
    std::span<const ast::GenericParam> params,
    const ast::TypeNode& declared) const {
  DeclaredBinding none{
      static_cast<u32>(params.size()), false, false, false, false, {}};
  if (declared.kind == ast::TypeKind::Ref) {
    const DeclaredBinding inner = declared_binding(
        params, ast.types[declared.payload.get<ast::TypeRef>().inner]);
    return DeclaredBinding{inner.slot,           true,
                           inner.through_uninit, inner.through_slice,
                           inner.through_raw,    inner.arg_at};
  }
  if (declared.kind == ast::TypeKind::RawPtr) {
    const DeclaredBinding inner = declared_binding(
        params, ast.types[declared.payload.get<ast::TypeRawPtr>().inner]);
    return DeclaredBinding{inner.slot,          false, inner.through_uninit,
                           inner.through_slice, true,  inner.arg_at};
  }
  if (declared.kind == ast::TypeKind::Slice) {
    // `&[T]` pins `T` from the array or slice behind the reference.
    const DeclaredBinding inner = declared_binding(
        params, ast.types[declared.payload.get<ast::TypeSlice>().element]);
    return DeclaredBinding{inner.slot, inner.through_ref, inner.through_uninit,
                           true,       inner.through_raw, inner.arg_at};
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
    return DeclaredBinding{
        inner.slot,          inner.through_ref, true,
        inner.through_slice, inner.through_raw, inner.arg_at};
  }
  if (!type_path.args.empty()) {
    // A slot inside a generic instance's arguments reads out of the
    // instance the argument names: the record is the whole path of
    // argument indices, outermost first.
    for (usize i = 0; i < type_path.args.size(); ++i) {
      DeclaredBinding inner =
          declared_binding(params, ast.types[type_path.args[i]]);
      if (inner.slot < params.size()) {
        inner.arg_at.insert(inner.arg_at.begin(), static_cast<u32>(i));
        return inner;
      }
    }
    return none;
  }
  if (path.segments.size() != 1) {
    return none;
  }
  return DeclaredBinding{param_slot(params, path.segments[0].name),
                         false,
                         false,
                         false,
                         false,
                         {}};
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
const CheckedModule::MethodInfo* Checker::instantiate_spec_method(
    u32 impl_module,
    u32 spec,
    std::span<const ir::TypeIdx> spec_args,
    std::span<const ir::TypeIdx> method_args,
    ir::TypeIdx self_type,
    const std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
    std::span<const ValueParam> value_scope,
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
          existing.self_type.idx == self_type.idx &&
          std::ranges::equal(existing.method_args, method_args)) {
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
    // arguments before its signature resolves, the way they do for an
    // inherent generic method.
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
    if (!spec_method_sig(spec, name, spec_args, self_type, impl_module,
                         method_args, declared_params, declared_ret,
                         declared_receiver)) {
      type_params = std::move(outer_scope);
      value_params = std::move(outer_values);
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
          DiagCode::TypeMismatch, method_node.span, name, entry.name);
      (void)index;
      type_params = std::move(outer_scope);
      value_params = std::move(outer_values);
      return nullptr;
    }
    add_function(impl_module, {method_node.payload.get<ast::ItemFn>().name.name,
                               params, ret, method_item})
        .is_method = true;
    CheckedModule::MethodInfo* entry = &add_method(
        impl_module, {self_type,
                      method_node.payload.get<ast::ItemFn>().name.name,
                      params,
                      ret,
                      receiver,
                      method_item,
                      false,
                      spec,
                      {method_args.begin(), method_args.end()}});
    // The definition-site check records under its own key: no
    // numbering slot is claimed and no body is checked here (ADR-0079).
    if (parametric_mode()) {
      entry->method_inst = PARAM_INST;
      type_params = std::move(outer_scope);
      value_params = std::move(outer_values);
      return entry;
    }
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
    value_params = std::move(outer_values);
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
      if (!prechecked_fns_.contains(method_item.idx)) {
        check_fn(module, method_item, &self_type);
      }
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

diag::Span Checker::item_name_span(ast::ItemIdx item) const {
  if (!item.is_valid()) {
    return {};
  }
  const ast::ItemNode& node = ast.items[item];
  switch (node.kind) {
    case ast::ItemKind::Fn: return node.payload.get<ast::ItemFn>().name.span;
    case ast::ItemKind::Intrinsic:
      return node.payload.get<ast::ItemIntrinsic>().name.span;
    case ast::ItemKind::Static:
      return node.payload.get<ast::ItemStatic>().name.span;
    case ast::ItemKind::Const:
      return node.payload.get<ast::ItemConst>().name.span;
    default: return {};
  }
}

diag::Span Checker::declaration_of(const PathValue& value) const {
  using Kind = PathValue::Kind;
  switch (value.kind) {
    case Kind::Local: return value.declaration;
    case Kind::Static:
      return value.object == nullptr ? diag::Span{}
                                     : item_name_span(value.object->item);
    case Kind::Function:
      return value.function == nullptr ? diag::Span{}
                                       : item_name_span(value.function->item);
    case Kind::AssocFunction:
      return value.method == nullptr ? diag::Span{}
                                     : item_name_span(value.method->item);
    case Kind::GenericFn: return item_name_span(value.generic_item);
    case Kind::UnitVariant:
    case Kind::TupleVariant: {
      if (value.enom == nullptr) {
        return {};
      }
      const ast::ItemNode& decl = ast.items[value.enom->item];
      return decl.payload.get<ast::ItemEnum>()
          .variants[value.variant]
          .name.span;
    }
    case Kind::Type:
      return value.enom == nullptr ? diag::Span{} : value.enom->span;
    case Kind::ValueParam: return {};
  }
  return {};
}

// Resolves an expression path and records the reference, so the
// language server reads the checker's resolution rather than repeating
// it. The impl below does the resolution.
bool Checker::resolve_value_path(u32 module,
                                 ast::ExprIdx expr,
                                 ast::PathIdx path,
                                 std::span<const ast::TypeIdx> type_args,
                                 std::span<const ast::ExprIdx> call_args,
                                 PathValue& out) {
  if (!resolve_value_path_impl(module, expr, path, type_args, call_args, out)) {
    return false;
  }
  const diag::Span declaration = declaration_of(out);
  if (declaration.length != 0) {
    modules[module].references.push_back({ast.paths[path].span, declaration});
  }
  return true;
}

// Resolves an expression path to its value meaning. Locals shadow
// everything and value parameters shadow module items; nominal type
// names resolve to Kind::Type so callers can report "found type"
// instead of "unknown".
bool Checker::resolve_value_path_impl(u32 module,
                                      ast::ExprIdx expr,
                                      ast::PathIdx path,
                                      std::span<const ast::TypeIdx> type_args,
                                      std::span<const ast::ExprIdx> call_args,
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
              DiagCode::InvalidCapture, node.segments[0].span, name);
          (void)index;
        } else if (local->capture != NO_CAPTURE &&
                   local->capture < bound.captures.size()) {
          bound.captures[local->capture].used = true;
        }
      }
      out.kind = PathValue::Kind::Local;
      out.type = local->type;
      out.declaration = local->span;
      return true;
    }
    // A value parameter is an ordinary value of its annotation type;
    // the splice records what lowering emits where the name is.
    if (const ValueParam* binding = value_param(name)) {
      // The resolution below can touch the scope stack, so the
      // binding is read out before it runs.
      const u32 binding_module = binding->module;
      const u64 value = binding->value;
      const ast::TypeIdx annotation = binding->annotation;
      const ir::TypeIdx type =
          resolve_type(binding_module, annotation, nullptr);
      if (is_error(type)) {
        return false;
      }
      CheckedModule& target = modules[module];
      const u32 index = static_cast<u32>(target.value_splices.size());
      if (target.value_splice_by_key
              .emplace(CheckedModule::value_splice_key(expr, cur_inst), index)
              .second) {
        target.value_splices.push_back({expr, cur_inst, value, type});
      }
      out.kind = PathValue::Kind::ValueParam;
      out.type = type;
      return true;
    }
    if (const CheckedModule::StaticInfo* info = lookup_static(module, name)) {
      out.kind = PathValue::Kind::Static;
      out.type = info->type;
      out.object = info;
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
    if (NominalEntry* entry = find_nominal_in_scope(module, name)) {
      out.kind = PathValue::Kind::Type;
      out.enom = entry;
      return true;
    }
    emit_unresolved(bag, DiagCode::UnknownValue, node.span, std_hints, "value",
                    name);
    return false;
  }
  if (node.segments.size() == 2) {
    const std::string_view head = node.segments[0].name;
    const std::string_view member = node.segments[1].name;
    const u32 child =
        module_walk_head(head) ? NO_MODULE : find_child_module(module, head);
    if (child != NO_MODULE || module_walk_head(head)) {
      u32 target = NO_MODULE;
      if (!walk_module_prefix(module, path, "value", target)) {
        return false;
      }
      if (const CheckedModule::StaticInfo* info =
              lookup_static(target, member)) {
        if (!check_item_access(module, info->module, info->is_pub, node.span,
                               member)) {
          return false;
        }
        out.kind = PathValue::Kind::Static;
        out.type = info->type;
        out.object = info;
        return true;
      }
      if (const CheckedModule::FnSig* fn = lookup_function(target, member)) {
        const bool is_pub = !fn->item.is_valid() || ast.items[fn->item].is_pub;
        if (!check_item_access(module, target, is_pub, node.span, member)) {
          return false;
        }
        out.kind = PathValue::Kind::Function;
        out.function = fn;
        return true;
      }
      // A qualified name reaches a generic function the way a bare one
      // does; its turbofish rides the call.
      if (ast::ItemIdx generic = lookup_generic_fn(target, member);
          generic.is_valid()) {
        if (!check_item_access(module, target, ast.items[generic].is_pub,
                               node.span, member)) {
          return false;
        }
        out.kind = PathValue::Kind::GenericFn;
        out.generic_item = generic;
        return true;
      }
      // `Enum::<T>::Variant` names the variant of one instance, the way
      // `Enum::assoc::<T>` names the member of one. Only a unit variant
      // needs it: a tuple variant takes the instance from its payload.
      const u32 variant_turbofish_at =
          ast.exprs[expr].kind == ast::ExprKind::Path
              ? ast.exprs[expr].payload.get<ast::ExprPath>().type_args_segment
              : base::INVALID_IDX;
      const bool variant_owner_args =
          !type_args.empty() && variant_turbofish_at == 0;
      const auto variant_owner_type = [&](NominalEntry* entry) -> ir::TypeIdx {
        if (!variant_owner_args || nominal_params(*entry).empty()) {
          return nominal_owner_type(entry);
        }
        const std::span<const ast::GenericParam> params =
            nominal_params(*entry);
        std::vector<ir::TypeIdx> args;
        args.reserve(type_args.size());
        for (usize k = 0; k < type_args.size(); ++k) {
          if (k < params.size() && is_value_param(ast, params[k])) {
            u64 value = 0;
            if (!resolve_value_argument(module, type_args[k],
                                        params[k].name.name, node.span,
                                        value)) {
              return error_type();
            }
            args.push_back(builder.const_type(value));
          } else {
            args.push_back(resolve_type(module, type_args[k], nullptr));
          }
        }
        return instantiate_generic(static_cast<u32>(entry - nominals.data()),
                                   args, node.span);
      };
      std::vector<VariantMatch> matches;
      if (find_variant_in(target, member, matches) && matches.size() == 1) {
        const ast::ItemNode& decl = ast.items[matches[0].enom->item];
        if (!check_item_access(module, matches[0].enom->module, decl.is_pub,
                               node.span, member)) {
          return false;
        }
        out.enom = matches[0].enom;
        out.variant = matches[0].variant;
        out.type = variant_owner_type(matches[0].enom);
        out.kind = decl.payload.get<ast::ItemEnum>()
                           .variants[matches[0].variant]
                           .fields.empty()
                       ? PathValue::Kind::UnitVariant
                       : PathValue::Kind::TupleVariant;
        return true;
      }
      emit_unresolved(bag, DiagCode::UnknownValue, node.span, std_hints,
                      "value", member);
      return false;
    }
    // Nominal prefix: `Enum::Variant` or `Type::assoc`.
    NominalEntry* nominal = find_nominal_in_scope(module, head);
    if (nominal != nullptr) {
      const ast::ItemNode& nominal_node = ast.items[nominal->item];
      // `Enum::<T>::Variant` names the variant of one instance, the way
      // `Enum::assoc::<T>` names the member of one. Only a unit variant
      // needs it: a tuple variant takes the instance from its payload.
      const u32 variant_turbofish_at =
          ast.exprs[expr].kind == ast::ExprKind::Path
              ? ast.exprs[expr].payload.get<ast::ExprPath>().type_args_segment
              : base::INVALID_IDX;
      const bool variant_owner_args =
          !type_args.empty() && variant_turbofish_at == 0;
      const auto variant_owner_type = [&](NominalEntry* entry) -> ir::TypeIdx {
        if (!variant_owner_args || nominal_params(*entry).empty()) {
          return nominal_owner_type(entry);
        }
        const std::span<const ast::GenericParam> params =
            nominal_params(*entry);
        std::vector<ir::TypeIdx> args;
        args.reserve(type_args.size());
        for (usize k = 0; k < type_args.size(); ++k) {
          if (k < params.size() && is_value_param(ast, params[k])) {
            u64 value = 0;
            if (!resolve_value_argument(module, type_args[k],
                                        params[k].name.name, node.span,
                                        value)) {
              return error_type();
            }
            args.push_back(builder.const_type(value));
          } else {
            args.push_back(resolve_type(module, type_args[k], nullptr));
          }
        }
        return instantiate_generic(static_cast<u32>(entry - nominals.data()),
                                   args, node.span);
      };
      if (nominal_node.kind == ast::ItemKind::Enum) {
        for (u32 i = 0;
             i < static_cast<u32>(
                     nominal_node.payload.get<ast::ItemEnum>().variants.size());
             ++i) {
          if (nominal_node.payload.get<ast::ItemEnum>().variants[i].name.name ==
              member) {
            out.enom = nominal;
            out.variant = i;
            out.type = variant_owner_type(nominal);
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
      // A turbofish after the owner (`P::<T>::id`) names the owner's
      // arguments; one after the member (`P::id::<U>`) names the
      // member's. The position decides, so a member turbofish no longer
      // reads as the owner's.
      const u32 turbofish_at =
          ast.exprs[expr].kind == ast::ExprKind::Path
              ? ast.exprs[expr].payload.get<ast::ExprPath>().type_args_segment
              : base::INVALID_IDX;
      const bool owner_args = !type_args.empty() && turbofish_at == 0;
      const bool member_args = !type_args.empty() && turbofish_at == 1;
      const bool generic_owner = !nominal_params(*nominal).empty();
      ir::TypeIdx self = error_type();
      if (generic_owner) {
        // `Name::<T>::member` names the member of one instantiation.
        if (!owner_args) {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerAssociatedFunctionNeedsTypeArguments>(
                  DiagCode::GenericArguments, node.span, head, head, member);
          (void)index;
          return false;
        }
        const std::span<const ast::GenericParam> params =
            nominal_params(*nominal);
        std::vector<ir::TypeIdx> args;
        args.reserve(type_args.size());
        for (usize i = 0; i < type_args.size(); ++i) {
          if (i < params.size() && is_value_param(ast, params[i])) {
            u64 value = 0;
            if (!resolve_value_argument(module, type_args[i],
                                        params[i].name.name, node.span,
                                        value)) {
              return false;
            }
            args.push_back(builder.const_type(value));
          } else {
            args.push_back(resolve_type(module, type_args[i], nullptr));
          }
        }
        self = instantiate_generic(static_cast<u32>(nominal - nominals.data()),
                                   args, node.span);
        if (is_error(self)) {
          return false;
        }
      } else if (owner_args) {
        const u32 index = bag.emit<i18n::Key::AnalyzerUnexpectedTypeArguments>(
            DiagCode::GenericArguments, node.span, head);
        (void)index;
        return false;
      } else {
        self = intern_nominal(*nominal);
      }
      if (member_args) {
        // `Name::member::<U>` on a generic associated function
        // instantiates it the way a method-call turbofish does. The
        // name is resolved before its call is read, so an unspelled
        // instantiation has no arguments to infer from here.
        std::span<const ast::ExprIdx> no_args;
        const CheckedModule::MethodInfo* method = instantiate_generic_method(
            module, self, member, type_args, no_args, node.span);
        if (method != nullptr &&
            method->receiver == CheckedModule::ReceiverKind::None) {
          out.kind = PathValue::Kind::AssocFunction;
          out.method = method;
          return true;
        }
      } else if (!call_args.empty()) {
        // A call's arguments bind an associated function's own
        // parameters the way they bind a method's: the declaration
        // pins what its parameter types spell, and an argument fills
        // what stays open. An attempt that fails reports why, so the
        // unresolved-name report below would only repeat it.
        const u32 before = bag.error_count();
        // The turbofish that reached here named the owner: only one
        // turbofish is parsed per path, and the branch above takes the
        // member's own. Passing the owner's arguments on would pin the
        // member's first generics to them.
        const CheckedModule::MethodInfo* method = instantiate_generic_method(
            module, self, member, /*type_args=*/{}, call_args, node.span);
        if (method != nullptr &&
            method->receiver == CheckedModule::ReceiverKind::None) {
          out.kind = PathValue::Kind::AssocFunction;
          out.method = method;
          return true;
        }
        if (bag.error_count() > before) {
          return false;
        }
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
    // A scalar or a bound type parameter as the prefix: `i64::parse`
    // and `T::parse` reach an associated function or a spec's
    // receiverless member (ADR-0122). A bound parameter sees the
    // instantiation's implementation, the way its receiver calls do
    // (ADR-0063).
    const ir::TypeIdx head_type = head_named_type(head);
    if (!is_error(head_type)) {
      if (const CheckedModule::MethodInfo* method =
              lookup_method(head_type, member, module, node.span, false)) {
        if (method->receiver == CheckedModule::ReceiverKind::None) {
          out.kind = PathValue::Kind::AssocFunction;
          out.method = method;
          return true;
        }
      }
    }
    // The hint names the package that carries `member` when one does,
    // so a path through the wrong staged prefix (`core::Vec`) points at
    // `collections`; the head answers only when the member is unknown.
    std::string_view known = member;
    std::string_view package = std_hint_package(std_hints, member);
    if (package.empty()) {
      known = head;
      package = std_hint_package(std_hints, head);
    }
    if (!package.empty()) {
      const u32 index =
          bag.emit<i18n::Key::AnalyzerUnresolvedValueInStandardLibrary>(
              DiagCode::UnknownValue, node.span, head, member, known, package);
      (void)index;
      return false;
    }
    const u32 index = bag.emit<i18n::Key::AnalyzerUnresolvedValue>(
        DiagCode::UnknownValue, node.span, head, member);
    (void)index;
    return false;
  }
  u32 target = NO_MODULE;
  if (!walk_module_prefix(module, path, "value", target)) {
    return false;
  }
  const std::string_view member = node.segments.back().name;
  if (const CheckedModule::StaticInfo* info = lookup_static(target, member)) {
    if (!check_item_access(module, info->module, info->is_pub, node.span,
                           member)) {
      return false;
    }
    out.kind = PathValue::Kind::Static;
    out.type = info->type;
    return true;
  }
  if (const CheckedModule::FnSig* fn = lookup_function(target, member)) {
    const bool is_pub = !fn->item.is_valid() || ast.items[fn->item].is_pub;
    if (!check_item_access(module, target, is_pub, node.span, member)) {
      return false;
    }
    out.kind = PathValue::Kind::Function;
    out.function = fn;
    return true;
  }
  if (ast::ItemIdx generic = lookup_generic_fn(target, member);
      generic.is_valid()) {
    if (!check_item_access(module, target, ast.items[generic].is_pub, node.span,
                           member)) {
      return false;
    }
    out.kind = PathValue::Kind::GenericFn;
    out.generic_item = generic;
    return true;
  }
  emit_unresolved(bag, DiagCode::UnknownValue, node.span, std_hints, "value",
                  member);
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
    const u32 child =
        module_walk_head(head) ? NO_MODULE : find_child_module(module, head);
    if (child != NO_MODULE || module_walk_head(head)) {
      u32 target = NO_MODULE;
      if (!walk_module_prefix(module, path, "value", target)) {
        return false;
      }
      std::vector<VariantMatch> matches;
      if (find_variant_in(target, member, matches) && matches.size() == 1) {
        const ast::ItemNode& enum_node = ast.items[matches[0].enom->item];
        if (!check_item_access(module, matches[0].enom->module,
                               enum_node.is_pub, node.span, member)) {
          return false;
        }
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
    emit_unresolved(bag, DiagCode::UnknownValue, node.span, std_hints, "struct",
                    node.segments[0].name);
    return nullptr;
  }
  u32 target = NO_MODULE;
  if (!walk_module_prefix(module, path, "value", target)) {
    return nullptr;
  }
  NominalEntry* nominal = find_nominal(target, node.segments.back().name);
  if (nominal != nullptr &&
      ast.items[nominal->item].kind == ast::ItemKind::Struct) {
    if (!check_item_access(module, nominal->module,
                           ast.items[nominal->item].is_pub, node.span,
                           node.segments.back().name)) {
      return nullptr;
    }
    return nominal;
  }
  emit_unresolved(bag, DiagCode::UnknownValue, node.span, std_hints, "struct",
                  node.segments.back().name);
  return nullptr;
}

bool Checker::contains_mut_ref(ir::TypeIdx idx, std::vector<u32>& visited) {
  for (u32 seen : visited) {
    if (seen == idx.idx) {
      return false;
    }
  }
  visited.push_back(idx.idx);
  const ir::TypeNode node = builder.types()[idx];
  switch (node.tag) {
    case ir::TypeTag::MutRef: return true;
    case ir::TypeTag::Ref:
      return contains_mut_ref(builder.ref_types()[node.as_ref()].pointee,
                              visited);
    case ir::TypeTag::Struct: {
      const ir::StructType struct_type =
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
      const ir::EnumType enum_type = builder.enum_types()[node.as_enum()];
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
      const ir::TupleType tuple = builder.tuple_types()[node.as_tuple()];
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

void Checker::check_fn(u32 module, ast::ItemIdx fn, const ir::TypeIdx* self) {
  const ast::ItemNode& node = ast.items[fn];
  // A body check can nest: the const pre-pass checks a called function
  // while an enclosing body is mid-check, so the per-function state is
  // saved and restored rather than left reset.
  const ir::TypeIdx saved_fn_ret = fn_ret;
  const bool saved_in_fn = in_fn;
  const u32 saved_loop_depth = loop_depth;
  const bool saved_reported_too_deep = reported_too_deep_;
  // One report per function: the budget is per function, and every
  // frame past it would otherwise repeat the same diagnostic.
  reported_too_deep_ = false;
  fn_ret = builder.primitive(ir::TypeTag::Void);
  if (node.payload.get<ast::ItemFn>().return_type.is_valid()) {
    fn_ret =
        resolve_type(module, node.payload.get<ast::ItemFn>().return_type, self);
  }
  // ADR-0085: a parameter and a return is Movable. A destructor's
  // receiver is the exception: the value ends in place.
  const bool is_destructor =
      self != nullptr && node.payload.get<ast::ItemFn>().name.name == "drop";
  if (!is_destructor && !is_error(fn_ret) && !is_movable(fn_ret)) {
    const u32 index = bag.emit<i18n::Key::AnalyzerUnmovableParameter>(
        DiagCode::UnmovableType, node.payload.get<ast::ItemFn>().name.span);
    (void)index;
  }
  in_fn = true;
  scopes.emplace_back();
  loop_depth = 0;
  for (const ast::ItemFnParam& param : node.payload.get<ast::ItemFn>().params) {
    // A variadic slot has no type to resolve and no pattern to bind;
    // only the compiler-expanded call reads the collected arguments
    // (ADR-0055), and the declared body guards with panic.
    if (param.is_variadic) {
      continue;
    }
    const ir::TypeIdx type = resolve_type(module, param.type, self);
    if (!is_destructor && !is_error(type) && !is_movable(type)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnmovableParameter>(
          DiagCode::UnmovableType, ast.patterns[param.pattern].span);
      (void)index;
    }
    bind_comp_known = param.is_comp;
    const bool refutable = bind_pattern(module, param.pattern, type);
    bind_comp_known = false;
    if (refutable) {
      const u32 index =
          bag.emit<i18n::Key::AnalyzerRefutablePatternInParameter>(
              DiagCode::RefutableLet, ast.patterns[param.pattern].span);
      (void)index;
    }
  }
  if (node.payload.get<ast::ItemFn>().body.is_valid()) {
    const ast::BlockIdx body = node.payload.get<ast::ItemFn>().body;
    // A unit body keeps the expectation, so a non-`()` trailing value
    // is still the explicit-discard error it was; a value-returning
    // body gets no expectation, because a non-unit return is spelled,
    // not inferred, and the body's own type has to diverge (ADR-0059).
    // `ret` inside unifies against `fn_ret` on its own either way.
    const ir::TypeIdx type =
        check_block(module, body, is_void(fn_ret) ? &fn_ret : nullptr);
    // A return-position `fn()` signature still waiting for the body's
    // answer reports through its own diagnostic, not through this one
    // (the anchor is a stand-in nobody can name).
    const bool awaited =
        !pending_own_fn_returns_.empty() &&
        pending_own_fn_returns_.back().stand_in.idx == fn_ret.idx;
    if (!is_void(fn_ret) && !awaited && !is_error(fn_ret) && !is_never(type) &&
        !is_error(type)) {
      const diag::Span span = ast.blocks[body].value.is_valid()
                                  ? ast.exprs[ast.blocks[body].value].span
                                  : ast.blocks[body].span;
      const u32 index = bag.emit<i18n::Key::AnalyzerReturnNeedsRet>(
          DiagCode::BadReturn, span, pretty_tag(tag_of(fn_ret)));
      (void)index;
    }
  }
  scopes.pop_back();
  in_fn = saved_in_fn;
  fn_ret = saved_fn_ret;
  loop_depth = saved_loop_depth;
  reported_too_deep_ = saved_reported_too_deep;
}

void Checker::check_main(u32 module, ast::ItemIdx fn) {
  const ast::ItemNode& node = ast.items[fn];
  if (!node.payload.get<ast::ItemFn>().params.empty()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerMainTakesParameters>(
        DiagCode::BadReturn, node.span);
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
    const ir::EnumType shape =
        builder.enum_types()[builder.types()[ret].as_enum()];
    if (shape.variants.size() == 2) {
      const ir::EnumVariantType first =
          builder.enum_variant_types()[shape.variants.head()];
      if (first.fields.size() == 1 && is_void(first.fields.head())) {
        return;
      }
    }
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerMainReturnType>(
      DiagCode::BadReturn, node.span);
  (void)index;
}

void Checker::check_bodies() {
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(
        profiler, trace_module_name(*tree.modules[m]), "analyze");
    for (ast::ItemIdx item : tree.modules[m]->items) {
      const ast::ItemNode& node = ast.items[item];
      switch (node.kind) {
        case ast::ItemKind::Fn: {
          // Generic bodies are checked per instantiation at their call
          // sites, where the type parameters are bound. A body the
          // const pre-pass already checked is not checked twice.
          if (node.payload.get<ast::ItemFn>().generic.empty() &&
              !prechecked_fns_.contains(item.idx)) {
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
          if (self_node.kind == ast::TypeKind::Str) {
            self = builder.primitive(ir::TypeTag::Str);
            self_ptr = &self;
          } else if (self_node.kind == ast::TypeKind::Path &&
                     self_node.payload.get<ast::TypePath>().args.empty()) {
            std::string_view target_name;
            if (NominalEntry* entry = find_impl_target(
                    m, node.payload.get<ast::ItemImpl>().type, target_name)) {
              if (nominal_params(*entry).empty()) {
                self = intern_nominal(*entry);
                self_ptr = &self;
              }
            }
          }
          for (ast::ItemIdx method :
               node.payload.get<ast::ItemImpl>().methods) {
            // A method that declares its own type parameters is checked
            // per instantiation, where the call spells them.
            if (!ast.items[method].payload.get<ast::ItemFn>().generic.empty()) {
              continue;
            }
            // A method a const initializer already reached had its body
            // typed on demand.
            if (!prechecked_fns_.contains(method.idx)) {
              check_fn(m, method, self_ptr);
            }
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
          if (!is_const) {
            std::vector<u32> visited;
            if (contains_mut_ref(declared, visited)) {
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerStaticHoldsMutableRef>(
                      DiagCode::InvalidOperation, ast.types[type].span, name);
              (void)index;
            }
          }
          // A const an array length already evaluated was checked on
          // demand; checking it again would repeat its diagnostics.
          if (is_const && const_init_checked_.contains(init.idx)) {
            break;
          }
          in_fn = false;
          scopes.emplace_back();
          // A const initializer is a compile-time value, so the same
          // restrictions as a comp block apply while it is checked.
          const bool entered_comp = is_const;
          if (entered_comp) {
            ++comp_depth;
          }
          const ir::TypeIdx actual = check_expr(m, init, &declared);
          if (entered_comp) {
            --comp_depth;
          }
          unify(declared, actual, ast.exprs[init].span, "item initializer");
          scopes.pop_back();
          break;
        }
        default: break;
      }
    }
  }
}

// --- The definition-site check (ADR-0079) -------------------------------

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
    ir::SymbolTable& strings,
    std::span<const StdHint> std_hints,
    debug::Profiler* profiler) {
  // Consumer precondition: the tree shape and every arena index the
  // checker dereferences are validated before any pass runs, so
  // hand-built trees fail with a diagnostic instead of UB.
  if (base::Result<void, ModuleTreeError> verified = verify_module_tree(tree);
      verified.is_err()) {
    const u32 index = bag.emit<i18n::Key::CodegenInvalidModuleTree>(
        DiagCode::InvalidModuleTree,
        describe_module_tree_error(std::move(verified).unwrap_err()));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  if (base::Result<void, ast::VerificationError> verified =
          ast::verify_file(ast);
      verified.is_err()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerInvalidSyntaxTree>(
        DiagCode::InvalidModuleTree,
        ast::describe_verification_error(std::move(verified).unwrap_err()));
    (void)index;
    return base::make_err(diag::Reported{});
  }

  // A member the program already staged cannot be added again, so a
  // hint naming one would send the user in circles; those hints are
  // dropped here, before name resolution reads them.
  std::vector<StdHint> usable_hints;
  usable_hints.reserve(std_hints.size());
  for (const StdHint& hint : std_hints) {
    bool staged = false;
    for (const ModuleNode* node : tree.modules) {
      if (node->is_staged && is_package_path(node->path, hint.package)) {
        staged = true;
        break;
      }
    }
    if (!staged) {
      usable_hints.push_back(hint);
    }
  }
  Checker checker{tree,    width,   ast,
                  bag,     strings, std::span<const StdHint>(usable_hints),
                  profiler};
  checker.uninit_name_id = checker.intern_name("MaybeUninit");
  if (checker.uninit_name_id == str::INVALID_STRING_POOL_ID) {
    return base::make_err(diag::Reported{});
  }
  checker.closure_name_id = checker.intern_name("Closure");
  if (checker.closure_name_id == str::INVALID_STRING_POOL_ID) {
    return base::make_err(diag::Reported{});
  }
  checker.method_param_name_id = checker.intern_name("MethodParam");
  if (checker.method_param_name_id == str::INVALID_STRING_POOL_ID) {
    return base::make_err(diag::Reported{});
  }
  checker.own_fn_return_stand_in_id = checker.intern_name("OwnFnReturn");
  if (checker.own_fn_return_stand_in_id == str::INVALID_STRING_POOL_ID) {
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
  checker.validate_generic_annotations();
  checker.validate_variadic_parameters();
  // Every slot exists before any module processes: a signature can
  // resolve a name into a module the sweep has not reached, and a slot
  // past the constructed range would be read unbuilt.
  checker.modules.resize(tree.modules.size());
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    checker.modules[m] = checker.empty_module(m);
  }
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(checker.profiler, "signatures",
                                             "analyze");
    for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(
          checker.profiler, trace_module_name(*tree.modules[m]), "analyze");
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
  // The definition-site check runs after every check that means a
  // concrete type: its scope binds the parameters and its records
  // carry PARAM_INST, which nothing else reads (ADR-0079).
  {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(checker.profiler, "generic bodies",
                                             "analyze");
    checker.check_generic_bodies();
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
            DiagCode::InvalidIr, diag::Span{}, ir::format_as(error.kind),
            error.index);
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
                         {},
                         {},
                         {}};
  package.drop_glue = std::move(checker.drop_glue_);
  package.needs_drop = std::move(checker.needs_drop_);
  package.not_movable = std::move(checker.not_movable_);
  package.comp_calls = std::move(checker.comp_calls_);
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
          {nominal.item, instance.type, std::move(fields)});
    } else {
      std::vector<std::string_view> variants;
      for (const ast::ItemEnumVariant& variant :
           node.payload.get<ast::ItemEnum>().variants) {
        variants.push_back(variant.name.name);
      }
      package.modules[nominal.module].enums.push_back(
          {nominal.item, nominal.name, instance.type, std::move(variants)});
    }
  }
  return base::make_ok(std::move(package));
}

}  // namespace analyzer
