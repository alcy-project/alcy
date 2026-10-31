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

// Whether a path head is one the module walk owns: the two relative
// forms and the two neighbor-language spellings that get their own
// message (ADR-0060).
bool module_walk_head(std::string_view head) {
  return head == "package" || head == "self" || head == "super" ||
         head == "crate";
}

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
      // A closure literal's nominal is the literal's own type, and the
      // signature it was checked against is what tells two literals
      // apart; spelling it is what keeps a mismatch of two literals
      // from reading as a mismatch of a type with itself.
      if (shape.name == closure_name_id) {
        if (const ClosureSig* sig = closure_sig_of_type(origin)) {
          std::string out(interner.get(closure_sig_head_id));
          for (usize i = 0; i < sig->params.size(); ++i) {
            if (i != 0) {
              out += ", ";
            }
            out += type_display_at(sig->params[i], depth + 1);
          }
          out += ") -> " + type_display_at(sig->ret, depth + 1);
          return out;
        }
      }
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
bool type_mentions_params(const ast::AstArena& ast,
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
  for (const ClosureSig& sig : closureTables_.closure_sigs) {
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

const Checker::ValueParam* Checker::value_param(std::string_view name) const {
  for (usize i = value_params.size(); i > 0; --i) {
    if (value_params[i - 1].name == name) {
      return &value_params[i - 1];
    }
  }
  return nullptr;
}

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
         name == "sys_read" || name == "error_kind" ||
         name == "str_from_parts" || name == "slice_len" ||
         name == "slice_from_parts" || name == "slice_from_parts_mut" ||
         name == "alloc" || name == "dealloc" || name == "elem_ptr" ||
         name == "elem_ref" || name == "size_of" || name == "align_of" ||
         name == "uninit_write" || name == "uninit_assume" ||
         name == "uninit_ref" || name == "ptr_offset" ||
         name == "ptr_offset_mut" || name == "drop_in_place" ||
         name == "slice_at" || name == "slice_at_mut" || name == "read" ||
         name == "wrapping_add" || name == "wrapping_sub" ||
         name == "wrapping_mul" || name == "file_size" ||
         name == "read_file_into" || name == "write_file_all" ||
         name == "arg_count" || name == "arg";
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
  } else if (name == "error_kind") {
    expected.push_back(builder.primitive(ir::TypeTag::I32));
    expected_ret = builder.primitive(ir::TypeTag::I32);
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
  // The pointer-width spellings name the target's width (ADR-0132).
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
  const auto cached = constInitTables_.const_int_values_.find(info.init.idx);
  if (cached != constInitTables_.const_int_values_.end()) {
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
  constInitTables_.const_int_values_.emplace(info.init.idx, out);
  return true;
}

bool Checker::ensure_const_checked(const CheckedModule::StaticInfo& info) {
  const u32 key = info.init.idx;
  if (constInitTables_.const_init_checked_.contains(key)) {
    return !constInitTables_.const_init_failed_.contains(key);
  }
  if (constInitTables_.const_init_in_progress_.contains(key)) {
    const u32 index = bag.emit<i18n::Key::AnalyzerConstCycle>(
        DiagCode::InvalidOperation, ast.exprs[info.init].span, info.name);
    (void)index;
    return false;
  }
  constInitTables_.const_init_in_progress_.insert(key);
  bool ok = check_const_dependencies(info.module, info.init);
  if (ok && !constInitTables_.const_init_checked_.contains(key)) {
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
      constInitTables_.const_init_failed_.insert(key);
    }
    constInitTables_.const_init_checked_.insert(key);
  }
  constInitTables_.const_init_in_progress_.erase(key);
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
  if (!fn.item.is_valid() ||
      constInitTables_.prechecked_fns_.contains(fn.item.idx)) {
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
  constInitTables_.prechecked_fns_.insert(fn.item.idx);
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
  if (!method.item.is_valid() ||
      constInitTables_.prechecked_fns_.contains(method.item.idx)) {
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
  constInitTables_.prechecked_fns_.insert(method.item.idx);
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
    // A method of a spec impl answers its call through this path, so
    // the declaration's bounds are checked here too: the impl may spell
    // the method's parameters more permissively than the spec does.
    if (impl.spec.is_valid()) {
      const SpecEntry* spec_entry = spec_of_impl_path(entry.module, impl.spec);
      if (spec_entry == nullptr) {
        return nullptr;
      }
      const u32 spec = static_cast<u32>(spec_entry - specs.data());
      if (!check_spec_method_bounds(spec, name, target_args, method_args,
                                    span)) {
        return nullptr;
      }
    }
    // The body check that follows answers each fresh stand-in: the
    // instantiation consumes the record and takes the witness for the
    // return type, or reports that none came.
    for (usize slot : return_slots) {
      ownFnTables_.pending_own_fn_returns_.push_back(
          PendingOwnFnReturn{entry.method_item, slot, method_args[slot],
                             own_fn_slot(method_fn.generic[slot].name.name),
                             span, witness_key(method_args, slot)});
    }
    return instantiate_method(entry.module, self, scope, value_scope, impl,
                              name, method_args);
  }
  return nullptr;
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
  for (const OwnFnSlot& slot : ownFnTables_.own_fn_slots_) {
    if (slot.name == name) {
      return &slot;
    }
  }
  return nullptr;
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
u32 param_slot(std::span<const ast::GenericParam> params,
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

const CheckedModule::MethodInfo* Checker::instantiate_spec_method(
    u32 impl_module,
    u32 spec,
    std::span<const ir::TypeIdx> spec_args,
    std::span<const ir::TypeIdx> method_args,
    ir::TypeIdx self_type,
    const std::vector<std::pair<std::string_view, ir::TypeIdx>>& scope,
    std::span<const ValueParam> value_scope,
    const ast::ItemImpl& impl,
    std::string_view name,
    diag::Span span) {
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
    // The declaration's bounds hold whatever the implementation wrote
    // down: an implementation may leave a bound out, and the call still
    // owes it (ADR-0028).
    const bool bounds_ok =
        check_spec_method_bounds(spec, name, spec_args, method_args, span);
    if (!bounds_ok) {
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
      if (!closureTables_.closure_bounds.empty()) {
        ClosureBound& bound = closureTables_.closure_bounds.back();
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
    // receiverless member (ADR-0133). A bound parameter sees the
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
        !ownFnTables_.pending_own_fn_returns_.empty() &&
        ownFnTables_.pending_own_fn_returns_.back().stand_in.idx == fn_ret.idx;
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
              !constInitTables_.prechecked_fns_.contains(item.idx)) {
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
            if (!constInitTables_.prechecked_fns_.contains(method.idx)) {
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
          if (is_const &&
              constInitTables_.const_init_checked_.contains(init.idx)) {
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
  checker.closure_sig_head_id = checker.intern_name("Closure(");
  if (checker.closure_sig_head_id == str::INVALID_STRING_POOL_ID) {
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
                         {},
                         {},
                         {}};
  package.drop_glue = std::move(checker.drop_glue_);
  package.needs_drop = std::move(checker.needs_drop_);
  package.not_movable = std::move(checker.not_movable_);
  package.comp_calls = std::move(checker.comp_calls_);
  package.specs.reserve(checker.specs.size());
  for (const SpecEntry& spec : checker.specs) {
    package.specs.push_back({spec.name, spec.item});
  }
  package.spec_impls.reserve(checker.spec_impls.size());
  for (const SpecImplEntry& impl : checker.spec_impls) {
    package.spec_impls.push_back({impl.spec, impl.item});
  }
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
