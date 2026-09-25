// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/types.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/checker.h"
#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {
Checker::Checker(const ModuleTree& tree,
                 ir::PointerWidth width,
                 ast::AstArena& ast,
                 diag::DiagBag& bag)
    : tree(tree),
      ast(ast),
      width(width),
      bag(bag),
      interner(kInternerCapacity) {}

// Pass 1: registers every nominal definition, diagnosing duplicates
// and reserved names. No interning happens here.
void Checker::register_nominals() {
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    for (ast::ItemIdx item : tree.modules[m]->items) {
      const ast::ItemNode& node = ast.items[item];
      if (node.kind != ast::ItemKind::Struct &&
          node.kind != ast::ItemKind::Enum) {
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
      bool duplicate = false;
      for (const NominalEntry& entry : nominals) {
        if (entry.module == m && entry.name == name) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate && name == "MaybeUninit") {
        // The wrapper is compiler-owned; a declaration under the same
        // name would make the spelling resolve two ways.
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerDuplicateDefinition, span,
                     "'{}' is a built-in type", name);
        (void)index;
        continue;
      }
      if (duplicate) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerDuplicateDefinition, span,
                     "duplicate definition of '{}'", name);
        (void)index;
        continue;
      }
      nominals.push_back(NominalEntry{m, name, item, span, ir::TypeIdx(0)});
    }
  }
}

NominalEntry* Checker::find_nominal(u32 module, std::string_view name) {
  for (NominalEntry& entry : nominals) {
    if (entry.module == module && entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

u32 Checker::find_child_module(u32 module, std::string_view name) const {
  for (u32 i = 0; i < static_cast<u32>(tree.modules.size()); ++i) {
    if (parents[i] != module) {
      continue;
    }
    const std::string& path = tree.modules[i]->path;
    const usize slash = path.find_last_of(':');
    const std::string_view tail =
        slash == std::string::npos ? std::string_view(path)
                                   : std::string_view(path).substr(slash + 1);
    if (tail == name) {
      return i;
    }
  }
  return kNoModule;
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
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnsupportedType, span,
                   "type is not supported in MVP");
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

// Interns a registered nominal, reserving its index first so recursive
// references resolve to it. A post-pass rejects uninhabited cycles.

// `MaybeUninit<T>` is a compiler-owned one-field struct standing for
// storage that holds a `T` nobody has written yet. It has the payload's
// layout and lowers transparently to it, so the wrapper costs nothing;
// its purpose is to keep the unwritten value out of reach until
// `uninit_assume` releases it.
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

// Payload of a `MaybeUninit<T>` wrapper, or an invalid index for any
// other type.
ir::TypeIdx Checker::uninit_payload(ir::TypeIdx type) const {
  if (tag_of(type) != ir::TypeTag::Struct) {
    return ir::TypeIdx::invalid();
  }
  const ir::StructType& shape =
      builder.struct_types()[builder.types()[type.idx].as_struct()];
  if (shape.name.offset != uninit_name_id.offset ||
      shape.name.length != uninit_name_id.length) {
    return ir::TypeIdx::invalid();
  }
  return shape.fields.size() == 1 ? shape.fields[0] : ir::TypeIdx::invalid();
}

ir::TypeIdx Checker::intern_nominal(NominalEntry& entry) {
  if (entry.complete || entry.started) {
    return entry.type;
  }
  const str::StringPoolId name = interner.intern(entry.name);
  const ast::ItemNode& node = ast.items[entry.item];
  if (node.kind == ast::ItemKind::Struct) {
    entry.type = builder.reserve_struct(name);
  } else {
    entry.type = builder.reserve_enum(name);
  }
  entry.started = true;
  if (node.kind == ast::ItemKind::Struct) {
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
    u32 index = 0;
    for (const ast::ItemEnumVariant& variant :
         node.payload.get<ast::ItemEnum>().variants) {
      ir::TypeSeq seq;
      for (ir::TypeIdx field : payloads[index]) {
        seq.push(storage_copy(field));
      }
      ++index;
      variants.push(builder.enum_variant(interner.intern(variant.name.name),
                                         seq.finish()));
    }
    builder.fill_enum(entry.type, variants.finish(), ir::TypeIdxRange{});
  }
  entry.complete = true;
  return entry.type;
}

const GenericInstance* Checker::generic_find(ir::TypeIdx idx) const {
  for (const GenericInstance& instance : generic_instances) {
    if (instance.type.idx == idx.idx) {
      return &instance;
    }
  }
  return nullptr;
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

// Appends a storage copy of `type` and records its origin.
ir::TypeIdx Checker::storage_copy(ir::TypeIdx type) {
  const ir::TypeIdx copy = builder.ref_type(type);
  type_origins_.emplace_back(copy, type);
  return copy;
}

// Key of a type in the shared instantiation numbering, which is what
// lowering side tables are keyed by. Returns kNoInst for a type that
// is not a generic instantiation.
u32 Checker::inst_index(ir::TypeIdx type) const {
  for (u32 i = 0; i < static_cast<u32>(inst_numbering.size()); ++i) {
    if (inst_numbering[i].idx == type.idx) {
      return i;
    }
  }
  return kNoInst;
}

// Storage copies chain: a copy's origin can itself be a copy, so the
// chain is followed to the type the first copy was made from.
ir::TypeIdx Checker::type_origin(ir::TypeIdx type) const {
  ir::TypeIdx current = type;
  for (u32 depth = 0; depth <= type_origins_.size(); ++depth) {
    const std::pair<ir::TypeIdx, ir::TypeIdx>* found = nullptr;
    for (usize i = type_origins_.size(); i > 0; --i) {
      if (type_origins_[i - 1].first.idx == current.idx) {
        found = &type_origins_[i - 1];
        break;
      }
    }
    if (found == nullptr) {
      return current;
    }
    current = found->second;
  }
  return current;
}

// Parameter names of a nominal declaration, whether struct or enum.
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

// Interns a nominal's type. A generic declaration defers: its type is
// fixed later against the expectation or the scrutinee, so the error
// type marks "resolve from context".
ir::TypeIdx Checker::nominal_owner_type(NominalEntry* entry) {
  if (!nominal_params(*entry).empty()) {
    return error_type();
  }
  return intern_nominal(*entry);
}

// Infers a generic instantiation from constructor arguments: a field
// whose declared type is exactly `T` binds `T` to that argument's type.
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
                                 ir::TypeIdx(base::kInvalidIdx));
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
  for (const GenericInstance& instance : generic_instances) {
    if (instance.nominal == nominal && instance.args == args) {
      return instance.type;
    }
  }
  const NominalEntry& entry = nominals[nominal];
  const ast::ItemNode& node = ast.items[entry.item];
  const str::StringPoolId name = interner.intern(entry.name);
  const bool is_struct = node.kind == ast::ItemKind::Struct;
  const ir::TypeIdx reserved =
      is_struct ? builder.reserve_struct(name) : builder.reserve_enum(name);
  generic_instances.push_back(GenericInstance{nominal, args, reserved});
  inst_numbering.push_back(reserved);
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
    ir::TypeSeq seq;
    for (const ast::ItemStructField& field :
         node.payload.get<ast::ItemStruct>().fields) {
      seq.push(storage_copy(resolve_type(entry.module, field.type, nullptr)));
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
    u32 index = 0;
    for (const ast::ItemEnumVariant& variant : decl.variants) {
      ir::TypeSeq seq;
      for (ir::TypeIdx field : payloads[index]) {
        seq.push(storage_copy(field));
      }
      ++index;
      variants.push(builder.enum_variant(interner.intern(variant.name.name),
                                         seq.finish()));
    }
    builder.fill_enum(reserved, variants.finish(), args_seq.finish());
  }
  while (type_params.size() > pushed) {
    type_params.pop_back();
  }
  GenericInstance& instance = generic_instances.back();
  instance.started = true;
  instance.complete = true;
  (void)span;
  return reserved;
}

// Resolves a type path to its defining module and member name.
// Resolves all path segments but the last to a module. Shared by
// type and value paths; `what` names the namespace for diagnostics.
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
  u32 current = kNoModule;
  if (head == "package") {
    current = tree.root;
  } else if (head == "self") {
    current = module;
  } else if (head == "super") {
    if (parents[module] == kNoModule) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                   ast.paths[path].span, "the root module has no parent");
      (void)index;
      return false;
    }
    current = parents[module];
  } else {
    current = find_child_module(module, head);
    if (current == kNoModule) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                   ast.paths[path].span, "unresolved {} '{}'", what, head);
      (void)index;
      return false;
    }
  }
  for (usize i = 1; i + 1 < segments.size(); ++i) {
    current = find_child_module(current, segments[i].name);
    if (current == kNoModule) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                                 ast.paths[path].span, "unresolved {} '{}'",
                                 what, segments[i].name);
      (void)index;
      return false;
    }
  }
  module_out = current;
  return true;
}

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
                                  const ir::TypeIdx* self) {
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
        seq.push(builder.ref_type(element));
      }
      return builder.tuple_type(seq.finish());
    }
    case ast::TypeKind::Array: {
      const ast::TypeArray& array = node.payload.get<ast::TypeArray>();
      const ir::TypeIdx element = resolve_type(module, array.element, self);
      return builder.array_type(element, array.count);
    }
    case ast::TypeKind::Ref: {
      ir::TypeIdx pointee =
          resolve_type(module, node.payload.get<ast::TypeRef>().inner, self);
      return builder.reference_type(pointee,
                                    node.payload.get<ast::TypeRef>().is_mut);
    }
    case ast::TypeKind::Path: {
      const ast::Path& path = ast.paths[node.payload.get<ast::TypePath>().path];
      if (path.segments.size() == 1) {
        const std::string_view name = path.segments[0].name;
        if (name == "Self") {
          if (self == nullptr) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerUnknownType, node.span,
                         "Self outside of an impl block");
            (void)index;
            return error_type();
          }
          if (!node.payload.get<ast::TypePath>().args.empty()) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerGenericArguments,
                         node.span, "generic arguments are not supported");
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
                  bag.emit(diag::Severity::Error, kAnalyzerGenericArguments,
                           node.span, "generic arguments are not supported");
              (void)index;
              return error_type();
            }
            return type_params[i - 1].second;
          }
        }
        if (name == "MaybeUninit") {
          const auto& args = node.payload.get<ast::TypePath>().args;
          if (args.size() != 1) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerGenericArguments,
                         node.span, "'MaybeUninit' takes one type argument");
            (void)index;
            return error_type();
          }
          return intern_uninit(resolve_type(module, args[0], self));
        }
      }
      u32 target_module = kNoModule;
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
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnknownType, node.span,
                     "'{}' is not a type", target_name);
        (void)index;
        return error_type();
      }
      const usize param_count = nominal_params(*entry).size();
      const usize arg_count = node.payload.get<ast::TypePath>().args.size();
      if (param_count == 0) {
        if (arg_count != 0) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerGenericArguments,
                       node.span, "generic arguments are not supported");
          (void)index;
          return error_type();
        }
        return intern_nominal(*entry);
      }
      if (arg_count != param_count) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityMismatch, node.span,
                     "'{}' expects {} argument{}", target_name, param_count,
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
         name == "str_from_parts" || name == "alloc" || name == "dealloc" ||
         name == "elem_ptr" || name == "size_of" || name == "align_of" ||
         name == "uninit_write" || name == "uninit_assume";
}

// Verifies a declared intrinsic signature against its canonical
// shape; declarations are documentation-checked, never trusted.
bool Checker::check_intrinsic_signature(u32 module,
                                        const ast::ItemIntrinsic& intrinsic,
                                        const std::vector<ir::TypeIdx>& params,
                                        ir::TypeIdx ret) {
  const std::string_view name = intrinsic.name.name;
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const ir::TypeIdx u8 = builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx usize_ty = builder.primitive(
      width == ir::PointerWidth::W64 ? ir::TypeTag::U64 : ir::TypeTag::U32);
  std::vector<ir::TypeIdx> expected;
  ir::TypeIdx expected_ret = builder.primitive(ir::TypeTag::Void);
  const auto wrong = [&]() {
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                               intrinsic.name.span,
                               "intrinsic '{}' has the wrong signature", name);
    (void)index;
    return false;
  };
  const auto is_usize = [&](ir::TypeIdx type) {
    return builder.types()[type.idx].tag == builder.types()[usize_ty.idx].tag;
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
        modules[module].functions.push_back(
            {node.payload.get<ast::ItemFn>().name.name, std::move(params), ret,
             item});
        break;
      }
      case ast::ItemKind::Intrinsic: {
        const ast::ItemIntrinsic& intrinsic =
            node.payload.get<ast::ItemIntrinsic>();
        if (!is_known_intrinsic(intrinsic.name.name)) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerUnknownIntrinsic,
                       intrinsic.name.span, "unknown intrinsic '{}'",
                       intrinsic.name.name);
          (void)index;
          break;
        }
        // A generic intrinsic registers per instantiation at its call
        // sites, but its shape is still checked here: binding each
        // parameter to a placeholder resolves the declaration to a
        // concrete signature the structural check can read.
        if (!intrinsic.generic.empty()) {
          const auto& kept = type_params;
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
                bag.emit(diag::Severity::Error, kAnalyzerInvalidComp,
                         ast.patterns[param.pattern].span,
                         "`comp` parameters on intrinsics are not supported");
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
        modules[module].functions.push_back(
            {intrinsic.name.name, std::move(params), ret, item});
        break;
      }
      case ast::ItemKind::Static:
      case ast::ItemKind::Const: {
        std::string_view name;
        ast::TypeIdx type = ast::TypeIdx::invalid();
        ast::ExprIdx init = ast::ExprIdx::invalid();
        const bool is_const = node.kind == ast::ItemKind::Const;
        if (!is_const) {
          name = node.payload.get<ast::ItemStatic>().name.name;
          type = node.payload.get<ast::ItemStatic>().type;
          init = node.payload.get<ast::ItemStatic>().init;
        } else {
          name = node.payload.get<ast::ItemConst>().name.name;
          type = node.payload.get<ast::ItemConst>().type;
          init = node.payload.get<ast::ItemConst>().init;
        }
        modules[module].statics.push_back(
            {name, resolve_type(module, type, nullptr), init, is_const});
        break;
      }
      case ast::ItemKind::Impl: {
        ir::TypeIdx self_type = error_type();
        bool self_ok = false;
        // Generic impls instantiate per method call; their methods
        // wait for instantiation-time checking.
        bool generic_impl = !node.payload.get<ast::ItemImpl>().params.empty();
        const ast::TypeNode& self_node =
            ast.types[node.payload.get<ast::ItemImpl>().type];
        if (self_node.kind == ast::TypeKind::Path) {
          if (!self_node.payload.get<ast::TypePath>().args.empty() &&
              node.payload.get<ast::ItemImpl>().params.empty()) {
            const u32 index = bag.emit(
                diag::Severity::Error, kAnalyzerGenericArguments,
                self_node.span, "generic impl blocks are not supported");
            (void)index;
          } else {
            u32 target_module = kNoModule;
            std::string_view target_name;
            if (resolve_type_path(module,
                                  self_node.payload.get<ast::TypePath>().path,
                                  target_module, target_name)) {
              NominalEntry* entry = find_nominal(target_module, target_name);
              if (entry != nullptr) {
                // Generic impls instantiate per method call; eager
                // registration cannot resolve their parameters yet.
                if (nominal_params(*entry).empty() &&
                    node.payload.get<ast::ItemImpl>().params.empty()) {
                  self_type = intern_nominal(*entry);
                  self_ok = true;
                } else {
                  generic_impl = true;
                }
              } else {
                const u32 index = bag.emit(
                    diag::Severity::Error, kAnalyzerUnknownType, self_node.span,
                    "inherent impl requires a nominal type");
                (void)index;
              }
            }
          }
        } else {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                       self_node.span, "inherent impl requires a nominal type");
          (void)index;
        }
        for (ast::ItemIdx method : node.payload.get<ast::ItemImpl>().methods) {
          if (generic_impl) {
            break;
          }
          const ast::ItemNode& method_node = ast.items[method];
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
          modules[module].functions.push_back(
              {method_node.payload.get<ast::ItemFn>().name.name,
               std::move(params), ret, method});
          const CheckedModule::FnSig& sig = modules[module].functions.back();
          CheckedModule::ReceiverKind receiver =
              CheckedModule::ReceiverKind::None;
          if (self_ok && !sig.params.empty()) {
            receiver = classify_receiver(sig.params[0], self_type);
          }
          modules[module].methods.push_back(
              {self_ok ? self_type : error_type(),
               method_node.payload.get<ast::ItemFn>().name.name, sig.params,
               sig.ret, receiver, method});
        }
        break;
      }
      case ast::ItemKind::Use: break;
    }
  }
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
    default: break;
  }
  stack.pop_back();
  return cycle;
}

std::string_view Checker::nominal_name(ir::TypeIdx idx) const {
  for (const NominalEntry& entry : nominals) {
    if (entry.complete && entry.type.idx == idx.idx) {
      return entry.name;
    }
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

// Structural type equality. Field slots hold copies (ranges demand
// consecutive fresh nodes), so index equality under-compares:
// primitives compare by tag, references/tuples/arrays recurse, and
// nominals (struct/enum) compare by index only. Every cycle passes
// through a nominal, but a seen-pair set guards regardless. Field
// slots hold storage copies, so both sides normalize to their origin
// before comparison; that makes a copied field type equal to the
// nominal it was copied from.
bool Checker::types_equal(ir::TypeIdx a, ir::TypeIdx b) {
  std::vector<u64> seen;
  return types_equal_inner(type_origin(a), type_origin(b), seen);
}

bool Checker::types_equal_inner(ir::TypeIdx a,
                                ir::TypeIdx b,
                                std::vector<u64>& seen) {
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
    case ir::TypeTag::MutRef: {
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
    case ir::TypeTag::Struct:
    case ir::TypeTag::Enum: return false;
    default: return true;
  }
}

// Unifies actual against expected, emitting a mismatch diagnostic.
// Never coerces to anything; Error suppresses follow-on diagnostics.
// Equality is structural: field slots hold copies, so shared shapes
// with different indexes still match.
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
  // An unwritten slot is the one mismatch with an obvious fix, so it
  // gets its own message instead of two type names.
  const ir::TypeIdx uninit_side = uninit_payload(expected).is_valid() ? expected
                                  : uninit_payload(actual).is_valid()
                                      ? actual
                                      : ir::TypeIdx::invalid();
  if (uninit_side.is_valid()) {
    const u32 index = bag.emit(
        diag::Severity::Error, kAnalyzerTypeMismatch, span,
        "uninitialized value used as '{}'; release it with "
        "'uninit_assume' once written",
        pretty_tag(tag_of(uninit_side == expected ? actual : expected)));
    (void)index;
    return error_type();
  }
  const u32 index =
      bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, span,
               "type mismatch in {}: expected '{}', found '{}'", what,
               pretty_tag(tag_of(expected)), pretty_tag(tag_of(actual)));
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

// Classifies an integer/float literal suffix. Returns true for a
// recognized suffix (tag set), false when absent (tag untouched).
// Suffixes mix letters and digits (`i32`, `usize`), so matching runs
// over known spellings from the end instead of scanning classes.
bool Checker::classify_suffix(std::string_view spelling,
                              ir::TypeTag& tag,
                              bool& is_float,
                              diag::Span span) {
  using TT = ir::TypeTag;
  struct Suffix {
    std::string_view text;
    ir::TypeTag tag;
    bool is_float;
  };
  constexpr Suffix kSuffixes[] = {
      {"isize", TT::I64, false}, {"usize", TT::U64, false},
      {"i8", TT::I8, false},     {"i16", TT::I16, false},
      {"i32", TT::I32, false},   {"i64", TT::I64, false},
      {"u8", TT::U8, false},     {"u16", TT::U16, false},
      {"u32", TT::U32, false},   {"u64", TT::U64, false},
      {"f32", TT::F32, true},    {"f64", TT::F64, true},
  };
  for (const Suffix& suffix : kSuffixes) {
    if (spelling.size() > suffix.text.size() &&
        spelling.substr(spelling.size() - suffix.text.size()) == suffix.text) {
      tag = suffix.tag;
      if (suffix.text == "isize") {
        tag = width == ir::PointerWidth::W64 ? TT::I64 : TT::I32;
      } else if (suffix.text == "usize") {
        tag = width == ir::PointerWidth::W64 ? TT::U64 : TT::U32;
      }
      is_float = suffix.is_float;
      return true;
    }
  }
  // A trailing alpha run that matches nothing known is an
  // unsupported suffix (`42i128`); pure digits have no suffix.
  usize alpha = spelling.size();
  while (alpha > 0 &&
         ((spelling[alpha - 1] >= 'a' && spelling[alpha - 1] <= 'z') ||
          (spelling[alpha - 1] >= 'A' && spelling[alpha - 1] <= 'Z'))) {
    --alpha;
  }
  if (alpha == spelling.size()) {
    return false;
  }
  const u32 index =
      bag.emit(diag::Severity::Error, kAnalyzerUnsupportedType, span,
               "unsupported literal suffix '{}'", spelling.substr(alpha));
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
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnsupportedType, lit.span,
                   "character literals need the core Char type (deferred)");
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
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, lit.span,
                     "float suffix on an integer literal");
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
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, lit.span,
                     "integer suffix on a float literal");
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
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerRecursiveType, entry.span,
                   "recursive type '{}' without indirection", entry.name);
      (void)index;
    }
  }
  for (const GenericInstance& instance : generic_instances) {
    if (!instance.complete) {
      continue;
    }
    std::vector<ir::TypeIdx> stack;
    if (has_value_cycle(instance.type, stack, storage)) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerRecursiveType,
                                 nominals[instance.nominal].span,
                                 "recursive type '{}' without indirection",
                                 nominals[instance.nominal].name);
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

// A generic free function item in scope, by name. Returns null when
// the name is not a generic function here or in an import.
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
    // lookup_generic_fn so each call rebinds its parameters.
    if (fn.name == name && fn_generic_params(fn.item).empty()) {
      return &fn;
    }
  }
  for (const Import& import : tree.modules[module]->imports) {
    if (import.ns != Namespace::Value || import.name != name) {
      continue;
    }
    for (const CheckedModule::FnSig& fn :
         modules[import.target_module].functions) {
      if (fn.name == import.member && fn_generic_params(fn.item).empty()) {
        return &fn;
      }
    }
  }
  return nullptr;
}

// Searches enum nominals of one module for a variant name.
bool Checker::find_variant_in(u32 module,
                              std::string_view name,
                              std::vector<VariantMatch>& out) {
  for (NominalEntry& entry : nominals) {
    if (entry.module != module) {
      continue;
    }
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
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                               span, "ambiguous variant '{}'", name);
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

const CheckedModule::MethodInfo* Checker::lookup_method(ir::TypeIdx self,
                                                        std::string_view name,
                                                        u32 module,
                                                        diag::Span span) {
  for (const CheckedModule& checked : modules) {
    for (const CheckedModule::MethodInfo& method : checked.methods) {
      if (method.self_type.idx == self.idx && method.name == name) {
        return &method;
      }
    }
  }
  // Generic instantiation: match `impl<...> Nominal<...>` blocks.
  const GenericInstance* instance = generic_find(self);
  if (instance == nullptr) {
    return nullptr;
  }
  const u32 nominal = instance->nominal;
  const std::vector<ir::TypeIdx> args = instance->args;
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
      u32 target_module = kNoModule;
      std::string_view target_name;
      if (!resolve_type_path(m, target.payload.get<ast::TypePath>().path,
                             target_module, target_name)) {
        continue;
      }
      NominalEntry* target_entry = find_nominal(target_module, target_name);
      if (target_entry == nullptr || nominal_index(target_entry) != nominal) {
        continue;
      }
      const std::span<const ast::TypeIdx> target_args =
          target.payload.get<ast::TypePath>().args;
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
  (void)module;
  (void)span;
  return nullptr;
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

// Declared parameters of a function or intrinsic item.
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

// Declared return type of a function or intrinsic item; invalid when
// the item declares none, which means `()`.
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
  const DeclaredBinding none{static_cast<u32>(params.size()), false, false};
  if (declared.kind == ast::TypeKind::Ref) {
    const DeclaredBinding inner = declared_binding(
        params, ast.types[declared.payload.get<ast::TypeRef>().inner]);
    return DeclaredBinding{inner.slot, true, inner.through_uninit};
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
    return DeclaredBinding{inner.slot, inner.through_ref, true};
  }
  if (!type_path.args.empty() || path.segments.size() != 1) {
    return none;
  }
  return DeclaredBinding{param_slot(params, path.segments[0].name), false,
                         false};
}

// Instantiates a generic function or intrinsic against `args` and
// checks its body. Returns null when the signature does not match the
// intrinsic's canonical shape.
const CheckedModule::FnSig* Checker::instantiate_fn(
    u32 module,
    ast::ItemIdx item,
    const std::vector<ir::TypeIdx>& args) {
  for (const FnInstance& instance : fn_instances) {
    if (instance.module == module && instance.item == item &&
        instance.args == args) {
      return &modules[module].functions[instance.sig_index];
    }
  }
  const bool is_intrinsic = ast.items[item].kind == ast::ItemKind::Intrinsic;
  const std::span<const ast::Ident> params = fn_generic_params(item);
  const auto& kept_outer = type_params;
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
  modules[module].functions.push_back(
      {fn_name(item), sig_params, ret, item, kNoInst});
  const u32 sig_index = static_cast<u32>(modules[module].functions.size()) - 1;
  // Claim a slot in the shared instantiation numbering before checking
  // the body, so recursive calls key the same context.
  fn_instances.push_back(FnInstance{item, module, args, sig_index, kNoInst});
  const u32 inst = static_cast<u32>(inst_numbering.size());
  inst_numbering.emplace_back(base::kInvalidIdx);
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
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerArityMismatch, span,
                 "'{}' expects {} type argument{}", fn_name(item),
                 params.size(), params.size() == 1 ? "" : "s");
    (void)index;
    return nullptr;
  }
  std::vector<ir::TypeIdx> bound(params.size(), ir::TypeIdx(base::kInvalidIdx));
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
    if (!declared.through_ref) {
      bound[declared.slot] = actual;
      continue;
    }
    const ir::TypeTag tag = builder.types()[actual.idx].tag;
    if (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                   "'{}' needs a reference to bind its type "
                   "argument",
                   fn_name(item));
      (void)index;
      return nullptr;
    }
    ir::TypeIdx bound_type =
        builder.ref_types()[builder.types()[actual.idx].as_ref()].pointee;
    if (declared.through_uninit) {
      bound_type = uninit_payload(bound_type);
      if (!bound_type.is_valid()) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                     "'{}' needs uninitialized storage to bind its type "
                     "argument",
                     fn_name(item));
        (void)index;
        return nullptr;
      }
    }
    bound[declared.slot] = bound_type;
  }
  for (const ir::TypeIdx arg : bound) {
    if (!arg.is_valid()) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                   "cannot infer the type arguments of '{}'; add them with "
                   "'{}::<T>(...)'",
                   fn_name(item), fn_name(item));
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
    modules[impl_module].functions.push_back(
        {method_node.payload.get<ast::ItemFn>().name.name, params, ret,
         method_item});
    modules[impl_module].methods.push_back(
        {self_type, method_node.payload.get<ast::ItemFn>().name.name, params,
         ret, receiver, method_item});
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

void Checker::record_call(u32 module,
                          ast::ExprIdx callee,
                          const CheckedModule::FnSig* fn) {
  for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
    for (u32 i = 0; i < static_cast<u32>(modules[m].functions.size()); ++i) {
      if (&modules[m].functions[i] == fn) {
        modules[module].call_targets.push_back({callee, false, m, i, cur_inst});
        return;
      }
    }
  }
}

void Checker::record_call(u32 module,
                          ast::ExprIdx callee,
                          const CheckedModule::MethodInfo* method) {
  for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
    for (u32 i = 0; i < static_cast<u32>(modules[m].methods.size()); ++i) {
      if (&modules[m].methods[i] == method) {
        modules[module].call_targets.push_back({callee, true, m, i, cur_inst});
        return;
      }
    }
  }
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
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                               node.span, "unresolved value '{}'", name);
    (void)index;
    return false;
  }
  if (node.segments.size() == 2) {
    const std::string_view head = node.segments[0].name;
    const std::string_view member = node.segments[1].name;
    const u32 child = (head == "package" || head == "self" || head == "super")
                          ? kNoModule
                          : find_child_module(module, head);
    if (child != kNoModule || head == "package" || head == "self" ||
        head == "super") {
      u32 target = kNoModule;
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
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                                 node.span, "unresolved value '{}'", member);
      (void)index;
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
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerGenericArguments, node.span,
              "'{}' needs type arguments to name an associated "
              "function; write '{}::<T>::{}'",
              head, head, member);
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
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerGenericArguments,
                     node.span, "'{}' takes no type arguments", head);
        (void)index;
        return false;
      }
      if (const CheckedModule::MethodInfo* method =
              lookup_method(self, member, module, node.span)) {
        if (method->receiver == CheckedModule::ReceiverKind::None) {
          out.kind = PathValue::Kind::AssocFunction;
          out.method = method;
          return true;
        }
      }
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerUnknownValue, node.span,
                 "unresolved value '{}::{}'", head, member);
    (void)index;
    return false;
  }
  u32 target = kNoModule;
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
  const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                             node.span, "unresolved value '{}'", member);
  (void)index;
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
                          ? kNoModule
                          : find_child_module(module, head);
    if (child != kNoModule || head == "package" || head == "self" ||
        head == "super") {
      u32 target = kNoModule;
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
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerUnknownValue, node.span,
                 "unresolved struct '{}'", node.segments[0].name);
    (void)index;
    return nullptr;
  }
  u32 target = kNoModule;
  if (!walk_module_prefix(module, path, "value", target)) {
    return nullptr;
  }
  NominalEntry* nominal = find_nominal(target, node.segments.back().name);
  if (nominal != nullptr &&
      ast.items[nominal->item].kind == ast::ItemKind::Struct) {
    return nominal;
  }
  const u32 index =
      bag.emit(diag::Severity::Error, kAnalyzerUnknownValue, node.span,
               "unresolved struct '{}'", node.segments.back().name);
  (void)index;
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

void Checker::check_fn(u32 module, ast::ItemIdx fn, const ir::TypeIdx* self) {
  const ast::ItemNode& node = ast.items[fn];
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
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerRefutableLet,
                                 ast.patterns[param.pattern].span,
                                 "refutable pattern in function parameter");
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
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadReturn,
                               node.span, "'main' must take no parameters");
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
  // first discriminant to exit code 0. See docs/adr/0009.
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
  const u32 index =
      bag.emit(diag::Severity::Error, kAnalyzerBadReturn, node.span,
               "'main' must return '()', 'i32', or a two-variant enum "
               "whose first variant holds '()'");
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
            u32 target_module = kNoModule;
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
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                         ast.exprs[init].span,
                         "const '{}' admits literal expressions only", name);
            (void)index;
          }
          if (!is_const) {
            std::vector<u32> visited;
            if (contains_mut_ref(declared, visited)) {
              const u32 index =
                  bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                           ast.types[type].span,
                           "static '{}' must not contain '&mut'", name);
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
diag::Fallible<CheckedPackage> check_package(const ModuleTree& tree,
                                             ir::PointerWidth width,
                                             ast::AstArena& ast,
                                             diag::DiagBag& bag) {
  Checker checker{tree, width, ast, bag};
  checker.uninit_name_id = checker.interner.intern("MaybeUninit");
  checker.register_nominals();
  checker.parents.assign(tree.modules.size(), kNoModule);
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    for (const ModuleNode* child : tree.modules[m]->children) {
      for (u32 c = 0; c < static_cast<u32>(tree.modules.size()); ++c) {
        if (tree.modules[c] == child) {
          checker.parents[c] = m;
          break;
        }
      }
    }
  }
  checker.modules.reserve(tree.modules.size());
  for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
    checker.modules.push_back(checker.empty_module(m));
    checker.process_module(m);
  }
  checker.check_bodies();
  ir::Storage storage = std::move(checker.builder).build();
  checker.validate_cycles(storage);
  CheckedPackage package{tree,
                         std::move(storage),
                         std::move(checker.modules),
                         std::move(checker.inst_numbering),
                         std::move(checker.fn_instances),
                         std::move(checker.type_origins_)};
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
