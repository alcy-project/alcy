// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "analyzer/types.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

namespace {

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

constexpr u32 kNoModule = 0xFFFFFFFFu;

struct NominalEntry {
  u32 module;
  std::string_view name;
  const ast::Item* item;
  diag::Span span;
  ir::TypeIdx type;
  bool started = false;
  bool complete = false;
};

struct BlessedEntry {
  bool is_result;
  std::vector<ir::TypeIdx> args;
  ir::TypeIdx type;
};

struct Checker {
  Checker(const ModuleTree& tree, ir::PointerWidth width, diag::DiagBag& bag)
      : tree(tree), width(width), bag(bag), interner(1024) {}

  const ModuleTree& tree;
  ir::PointerWidth width;
  diag::DiagBag& bag;
  ir::StorageBuilder builder;
  str::StringInterner interner;
  std::vector<NominalEntry> nominals;
  std::vector<BlessedEntry> blessed;
  std::vector<CheckedModule> modules;
  std::vector<u32> parents;

  // Body-checking state, reset per function.
  struct Local {
    std::string_view name;
    ir::TypeIdx type;
    bool is_mut;
  };
  bool in_fn = false;

  std::vector<std::vector<Local>> scopes;
  u32 loop_depth = 0;
  ir::TypeIdx fn_ret = ir::TypeIdx(0);

  // Pass 1: registers every nominal definition, diagnosing duplicates
  // and reserved names. No interning happens here.
  void register_nominals() {
    for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
      for (ast::Item* item : tree.modules[m]->items) {
        if (item->kind != ast::ItemKind::Struct &&
            item->kind != ast::ItemKind::Enum) {
          continue;
        }
        std::string_view name;
        diag::Span span;
        if (item->kind == ast::ItemKind::Struct) {
          const ast::StructItem* decl = static_cast<ast::StructItem*>(item);
          name = decl->name.name;
          span = decl->name.span;
        } else {
          const ast::EnumItem* decl = static_cast<ast::EnumItem*>(item);
          name = decl->name.name;
          span = decl->name.span;
        }
        if (name == "Result" || name == "Option") {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerReservedName, span,
                       "'{}' is reserved for the blessed type", name);
          (void)index;
          continue;
        }
        bool duplicate = false;
        for (const NominalEntry& entry : nominals) {
          if (entry.module == m && entry.name == name) {
            duplicate = true;
            break;
          }
        }
        if (duplicate) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerDuplicateDefinition,
                       span, "duplicate definition of '{}'", name);
          (void)index;
          continue;
        }
        nominals.push_back(NominalEntry{m, name, item, span, ir::TypeIdx(0)});
      }
    }
  }

  NominalEntry* find_nominal(u32 module, std::string_view name) {
    for (NominalEntry& entry : nominals) {
      if (entry.module == module && entry.name == name) {
        return &entry;
      }
    }
    return nullptr;
  }

  u32 find_child_module(u32 module, std::string_view name) const {
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

  ir::TypeIdx primitive_type(ast::PrimitiveKind kind, diag::Span span) {
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

  ir::TypeIdx error_type() { return builder.error_type(); }

  // Derives the receiver kind from a resolved first parameter: exactly
  // Self, &Self, or &mut Self count; anything else is an associated
  // function regardless of the parameter name.
  CheckedModule::ReceiverKind classify_receiver(ir::TypeIdx first,
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
  ir::TypeIdx intern_nominal(NominalEntry& entry) {
    if (entry.complete || entry.started) {
      return entry.type;
    }
    const str::StringPoolId name = interner.intern(entry.name);
    if (entry.item->kind == ast::ItemKind::Struct) {
      entry.type = builder.reserve_struct(name);
    } else {
      entry.type = builder.reserve_enum(name);
    }
    entry.started = true;
    if (entry.item->kind == ast::ItemKind::Struct) {
      const ast::StructItem* decl =
          static_cast<const ast::StructItem*>(entry.item);
      std::vector<ir::TypeIdx> fields;
      fields.reserve(decl->fields.size());
      for (const ast::StructField& field : decl->fields) {
        // Exclusive references are legal fields; the structural Copy
        // rule marks the aggregate move-only.
        fields.push_back(resolve_type(entry.module, field.type, nullptr));
      }
      ir::TypeSeq seq;
      for (ir::TypeIdx field : fields) {
        seq.push(builder.ref_type(field));
      }
      builder.fill_struct(entry.type, seq.finish());
    } else {
      const ast::EnumItem* decl = static_cast<const ast::EnumItem*>(entry.item);
      // Payloads resolve first so variant nodes append back-to-back.
      std::vector<std::vector<ir::TypeIdx>> payloads;
      payloads.reserve(decl->variants.size());
      for (const ast::EnumVariant& variant : decl->variants) {
        std::vector<ir::TypeIdx> fields;
        fields.reserve(variant.fields.size());
        for (const ast::Type* field : variant.fields) {
          fields.push_back(resolve_type(entry.module, field, nullptr));
        }
        payloads.push_back(std::move(fields));
      }
      ir::EnumVariantTypeSeq variants;
      u32 index = 0;
      for (const ast::EnumVariant& variant : decl->variants) {
        ir::TypeSeq seq;
        for (ir::TypeIdx field : payloads[index]) {
          seq.push(builder.ref_type(field));
        }
        ++index;
        variants.push(builder.enum_variant(interner.intern(variant.name.name),
                                           seq.finish()));
      }
      builder.fill_enum(entry.type, variants.finish());
    }
    entry.complete = true;
    return entry.type;
  }

  ir::TypeIdx intern_blessed(bool is_result,
                             const std::vector<ir::TypeIdx>& args) {
    for (const BlessedEntry& entry : blessed) {
      if (entry.is_result == is_result && entry.args == args) {
        return entry.type;
      }
    }
    const char* first = is_result ? "Ok" : "Some";
    const char* second = is_result ? "Err" : "None";
    ir::EnumVariantTypeSeq variants;
    variants.push(builder.enum_variant(interner.intern(first), {args[0], 1}));
    if (is_result) {
      variants.push(
          builder.enum_variant(interner.intern(second), {args[1], 1}));
    } else {
      ir::TypeSeq empty;
      variants.push(
          builder.enum_variant(interner.intern(second), empty.finish()));
    }
    ir::TypeIdx type = builder.enum_type(
        interner.intern(is_result ? "Result" : "Option"), variants.finish());
    blessed.push_back(BlessedEntry{is_result, args, type});
    return type;
  }

  // Resolves a type path to its defining module and member name.
  // Resolves all path segments but the last to a module. Shared by
  // type and value paths; `what` names the namespace for diagnostics.
  bool walk_module_prefix(u32 module,
                          const ast::Path* path,
                          std::string_view what,
                          u32& module_out) {
    const std::string_view head = path->segments[0].name;
    if (path->segments.size() == 1) {
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
        const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                                   path->span, "the root module has no parent");
        (void)index;
        return false;
      }
      current = parents[module];
    } else {
      current = find_child_module(module, head);
      if (current == kNoModule) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnknownType, path->span,
                     "unresolved {} '{}'", what, head);
        (void)index;
        return false;
      }
    }
    for (usize i = 1; i + 1 < path->segments.size(); ++i) {
      current = find_child_module(current, path->segments[i].name);
      if (current == kNoModule) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnknownType, path->span,
                     "unresolved {} '{}'", what, path->segments[i].name);
        (void)index;
        return false;
      }
    }
    module_out = current;
    return true;
  }

  bool resolve_type_path(u32 module,
                         const ast::Path* path,
                         u32& module_out,
                         std::string_view& name_out) {
    if (path->segments.empty()) {
      return false;
    }
    if (!walk_module_prefix(module, path, "type", module_out)) {
      return false;
    }
    name_out = path->segments.back().name;
    return true;
  }

  ir::TypeIdx resolve_type(u32 module,
                           const ast::Type* type,
                           const ir::TypeIdx* self) {
    switch (type->kind) {
      case ast::TypeKind::Primitive: {
        const ast::PrimitiveType* primitive =
            static_cast<const ast::PrimitiveType*>(type);
        return primitive_type(primitive->primitive, type->span);
      }
      case ast::TypeKind::Unit: return builder.primitive(ir::TypeTag::Void);
      case ast::TypeKind::Never: return builder.never_type();
      case ast::TypeKind::Str: return builder.primitive(ir::TypeTag::Str);
      case ast::TypeKind::Tuple: {
        const ast::TupleType* tuple = static_cast<const ast::TupleType*>(type);
        std::vector<ir::TypeIdx> elements;
        elements.reserve(tuple->elements.size());
        for (const ast::Type* element : tuple->elements) {
          elements.push_back(resolve_type(module, element, self));
        }
        ir::TypeSeq seq;
        for (ir::TypeIdx element : elements) {
          seq.push(builder.ref_type(element));
        }
        return builder.tuple_type(seq.finish());
      }
      case ast::TypeKind::Ref: {
        const ast::RefType* ref = static_cast<const ast::RefType*>(type);
        ir::TypeIdx pointee = resolve_type(module, ref->inner, self);
        return builder.reference_type(pointee, ref->is_mut);
      }
      case ast::TypeKind::Path: {
        const ast::PathType* path = static_cast<const ast::PathType*>(type);
        if (path->path->segments.size() == 1) {
          const std::string_view name = path->path->segments[0].name;
          if (name == "Self") {
            if (self == nullptr) {
              const u32 index =
                  bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                           type->span, "Self outside of an impl block");
              (void)index;
              return error_type();
            }
            if (!path->args.empty()) {
              const u32 index =
                  bag.emit(diag::Severity::Error, kAnalyzerGenericArguments,
                           type->span, "generic arguments are not supported");
              (void)index;
              return error_type();
            }
            return *self;
          }
          if (name == "Result" || name == "Option") {
            const bool is_result = name == "Result";
            const usize want = is_result ? 2 : 1;
            if (path->args.size() != want) {
              const u32 index =
                  bag.emit(diag::Severity::Error, kAnalyzerArityMismatch,
                           type->span, "'{}' expects {} argument{}", name, want,
                           want == 1 ? "" : "s");
              (void)index;
              return error_type();
            }
            std::vector<ir::TypeIdx> args;
            args.reserve(path->args.size());
            for (const ast::Type* arg : path->args) {
              args.push_back(resolve_type(module, arg, self));
            }
            return intern_blessed(is_result, args);
          }
        }
        if (!path->args.empty()) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerGenericArguments,
                       type->span, "generic arguments are not supported");
          (void)index;
          return error_type();
        }
        u32 target_module = kNoModule;
        std::string_view target_name;
        if (!resolve_type_path(module, path->path, target_module,
                               target_name)) {
          return error_type();
        }
        // Single-segment names resolve through imports as well.
        NominalEntry* entry = find_nominal(target_module, target_name);
        if (entry == nullptr && path->path->segments.size() == 1) {
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
              bag.emit(diag::Severity::Error, kAnalyzerUnknownType, type->span,
                       "'{}' is not a type", target_name);
          (void)index;
          return error_type();
        }
        return intern_nominal(*entry);
      }
    }
  }

  void process_module(u32 module) {
    for (ast::Item* item : tree.modules[module]->items) {
      switch (item->kind) {
        case ast::ItemKind::Struct:
        case ast::ItemKind::Enum: {
          NominalEntry* entry = nullptr;
          if (item->kind == ast::ItemKind::Struct) {
            const ast::StructItem* decl =
                static_cast<const ast::StructItem*>(item);
            entry = find_nominal(module, decl->name.name);
          } else {
            const ast::EnumItem* decl = static_cast<const ast::EnumItem*>(item);
            entry = find_nominal(module, decl->name.name);
          }
          if (entry != nullptr) {
            const ir::TypeIdx resolved = intern_nominal(*entry);
            modules[module].types.push_back({entry->name, resolved});
            if (item->kind == ast::ItemKind::Struct) {
              const ast::StructItem* decl =
                  static_cast<const ast::StructItem*>(item);
              std::vector<std::string_view> fields;
              for (const ast::StructField& field : decl->fields) {
                fields.push_back(field.name.name);
              }
              modules[module].structs.push_back({resolved, std::move(fields)});
            }
          }
          break;
        }
        case ast::ItemKind::Fn: {
          const ast::FnItem* fn = static_cast<const ast::FnItem*>(item);
          std::vector<ir::TypeIdx> params;
          for (const ast::FnParam& param : fn->params) {
            params.push_back(resolve_type(module, param.type, nullptr));
          }
          ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
          if (fn->return_type != nullptr) {
            ret = resolve_type(module, fn->return_type, nullptr);
          }
          modules[module].functions.push_back(
              {fn->name.name, std::move(params), ret});
          break;
        }
        case ast::ItemKind::Static:
        case ast::ItemKind::Const: {
          std::string_view name;
          const ast::Type* type = nullptr;
          if (item->kind == ast::ItemKind::Static) {
            const ast::StaticItem* decl =
                static_cast<const ast::StaticItem*>(item);
            name = decl->name.name;
            type = decl->type;
          } else {
            const ast::ConstItem* decl =
                static_cast<const ast::ConstItem*>(item);
            name = decl->name.name;
            type = decl->type;
          }
          modules[module].statics.push_back(
              {name, resolve_type(module, type, nullptr)});
          break;
        }
        case ast::ItemKind::Impl: {
          const ast::ImplItem* impl = static_cast<const ast::ImplItem*>(item);
          ir::TypeIdx self_type = error_type();
          bool self_ok = false;
          if (impl->type->kind == ast::TypeKind::Path) {
            const ast::PathType* path =
                static_cast<const ast::PathType*>(impl->type);
            if (!path->args.empty()) {
              const u32 index = bag.emit(
                  diag::Severity::Error, kAnalyzerGenericArguments,
                  impl->type->span, "generic impl blocks are not supported");
              (void)index;
            } else {
              u32 target_module = kNoModule;
              std::string_view target_name;
              if (resolve_type_path(module, path->path, target_module,
                                    target_name)) {
                NominalEntry* entry = find_nominal(target_module, target_name);
                if (entry != nullptr) {
                  self_type = intern_nominal(*entry);
                  self_ok = true;
                } else {
                  const u32 index =
                      bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                               impl->type->span,
                               "inherent impl requires a nominal type");
                  (void)index;
                }
              }
            }
          } else {
            const u32 index = bag.emit(diag::Severity::Error,
                                       kAnalyzerUnknownType, impl->type->span,
                                       "inherent impl requires a nominal type");
            (void)index;
          }
          for (const ast::FnItem* method : impl->methods) {
            std::vector<ir::TypeIdx> params;
            for (const ast::FnParam& param : method->params) {
              params.push_back(resolve_type(module, param.type,
                                            self_ok ? &self_type : nullptr));
            }
            ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
            if (method->return_type != nullptr) {
              ret = resolve_type(module, method->return_type,
                                 self_ok ? &self_type : nullptr);
            }
            modules[module].functions.push_back(
                {method->name.name, std::move(params), ret});
            const CheckedModule::FnSig& sig = modules[module].functions.back();
            CheckedModule::ReceiverKind receiver =
                CheckedModule::ReceiverKind::None;
            if (self_ok && !sig.params.empty()) {
              receiver = classify_receiver(sig.params[0], self_type);
            }
            modules[module].methods.push_back(
                {self_ok ? self_type : error_type(), method->name.name,
                 sig.params, sig.ret, receiver});
          }
          break;
        }
        case ast::ItemKind::Mod:
        case ast::ItemKind::Use: break;
      }
    }
  }

  bool has_value_cycle(ir::TypeIdx root,
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
             vidx.idx <
             enum_type.variants.head().idx + enum_type.variants.size();
             vidx = ir::EnumVariantTypeIdx(vidx.idx + 1)) {
          const ir::EnumVariantType& variant =
              storage.enum_variant_types()[vidx];
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

  std::string_view nominal_name(ir::TypeIdx idx) const {
    for (const NominalEntry& entry : nominals) {
      if (entry.complete && entry.type.idx == idx.idx) {
        return entry.name;
      }
    }
    for (const BlessedEntry& entry : blessed) {
      if (entry.type.idx == idx.idx) {
        return entry.is_result ? "Result" : "Option";
      }
    }
    return "type";
  }

  // ---- Expression checking (Phase B4) ----

  ir::TypeTag tag_of(ir::TypeIdx idx) const { return builder.types()[idx].tag; }

  bool is_integer_tag(ir::TypeTag tag) const {
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

  bool is_float_tag(ir::TypeTag tag) const {
    return tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64;
  }

  bool is_void(ir::TypeIdx idx) const {
    return tag_of(idx) == ir::TypeTag::Void;
  }

  bool is_never(ir::TypeIdx idx) const {
    return tag_of(idx) == ir::TypeTag::Never;
  }

  bool is_error(ir::TypeIdx idx) const {
    return tag_of(idx) == ir::TypeTag::Error;
  }

  // Structural type equality. Field slots hold copies (ranges demand
  // consecutive fresh nodes), so index equality under-compares:
  // primitives compare by tag, references/tuples/arrays recurse, and
  // nominals (struct/enum) compare by index only. Every cycle passes
  // through a nominal, but a seen-pair set guards regardless.
  bool types_equal(ir::TypeIdx a, ir::TypeIdx b) {
    std::vector<u64> seen;
    return types_equal_inner(a, b, seen);
  }

  bool types_equal_inner(ir::TypeIdx a, ir::TypeIdx b, std::vector<u64>& seen) {
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

  const BlessedEntry* blessed_find(ir::TypeIdx idx) const {
    for (const BlessedEntry& entry : blessed) {
      if (entry.type.idx == idx.idx) {
        return &entry;
      }
    }
    return nullptr;
  }

  bool is_must_use(ir::TypeIdx idx) const {
    return blessed_find(idx) != nullptr;
  }

  // Unifies actual against expected, emitting a mismatch diagnostic.
  // Never coerces to anything; Error suppresses follow-on diagnostics.
  // Equality is structural: field slots hold copies, so shared shapes
  // with different indexes still match.
  ir::TypeIdx unify(ir::TypeIdx expected,
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
    const u32 index = bag.emit(
        diag::Severity::Error, kAnalyzerTypeMismatch, span,
        "type mismatch in {}: expected '{}', found '{}'", what,
        ir::type_to_str(tag_of(expected)), ir::type_to_str(tag_of(actual)));
    (void)index;
    return error_type();
  }

  const Local* lookup_local(std::string_view name) const {
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
  bool classify_suffix(std::string_view spelling,
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
          spelling.substr(spelling.size() - suffix.text.size()) ==
              suffix.text) {
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

  ir::TypeIdx check_literal(const ast::Literal* lit,
                            const ir::TypeIdx* expected) {
    using LK = ast::LiteralKind;
    switch (lit->kind) {
      case LK::Bool: {
        const ir::TypeIdx type = builder.primitive(ir::TypeTag::I1);
        if (expected != nullptr) {
          return unify(*expected, type, lit->span, "boolean literal");
        }
        return type;
      }
      case LK::String: {
        const ir::TypeIdx type = builder.primitive(ir::TypeTag::Str);
        if (expected != nullptr) {
          return unify(*expected, type, lit->span, "string literal");
        }
        return type;
      }
      case LK::Char: {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnsupportedType, lit->span,
                     "character literals need the core Char type (deferred)");
        (void)index;
        return error_type();
      }
      case LK::Integer: {
        ir::TypeTag tag = ir::TypeTag::I32;
        bool is_float = false;
        const bool has_suffix =
            classify_suffix(lit->spelling, tag, is_float, lit->span);
        if (tag == ir::TypeTag::Error) {
          return error_type();
        }
        if (is_float) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, lit->span,
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
          return unify(*expected, type, lit->span, "integer literal");
        }
        return type;
      }
      case LK::Float: {
        ir::TypeTag tag = ir::TypeTag::F64;
        bool is_float = true;
        const bool has_suffix =
            classify_suffix(lit->spelling, tag, is_float, lit->span);
        if (tag == ir::TypeTag::Error) {
          return error_type();
        }
        if (has_suffix && !is_float_tag(tag)) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, lit->span,
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
          return unify(*expected, type, lit->span, "float literal");
        }
        return type;
      }
    }
  }

  void validate_cycles(const ir::Storage& storage) {
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
  }

  // ---- Value namespace ----

  const CheckedModule::StaticInfo* lookup_static(u32 module,
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

  const CheckedModule::FnSig* lookup_function(u32 module,
                                              std::string_view name) const {
    for (const CheckedModule::FnSig& fn : modules[module].functions) {
      if (fn.name == name) {
        return &fn;
      }
    }
    for (const Import& import : tree.modules[module]->imports) {
      if (import.ns != Namespace::Value || import.name != name) {
        continue;
      }
      for (const CheckedModule::FnSig& fn :
           modules[import.target_module].functions) {
        if (fn.name == import.member) {
          return &fn;
        }
      }
    }
    return nullptr;
  }

  struct VariantMatch {
    NominalEntry* enom = nullptr;
    u32 variant = 0;
  };

  // Searches enum nominals of one module for a variant name.
  bool find_variant_in(u32 module,
                       std::string_view name,
                       std::vector<VariantMatch>& out) {
    for (NominalEntry& entry : nominals) {
      if (entry.module != module || entry.item->kind != ast::ItemKind::Enum) {
        continue;
      }
      const ast::EnumItem* decl = static_cast<const ast::EnumItem*>(entry.item);
      for (u32 i = 0; i < static_cast<u32>(decl->variants.size()); ++i) {
        if (decl->variants[i].name.name == name) {
          out.push_back({&entry, i});
        }
      }
    }
    return !out.empty();
  }

  // Variant lookup in scope: current module plus type-namespace imports.
  // Ambiguity diagnoses; unknown returns false silently for the caller
  // to report in context.
  bool find_variant(u32 module,
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
      if (target != nullptr && target->item->kind == ast::ItemKind::Enum) {
        const ast::EnumItem* decl =
            static_cast<const ast::EnumItem*>(target->item);
        for (u32 i = 0; i < static_cast<u32>(decl->variants.size()); ++i) {
          if (decl->variants[i].name.name == name) {
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

  NominalEntry* find_nominal_in_scope(u32 module, std::string_view name) {
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

  const CheckedModule::MethodInfo* lookup_method(ir::TypeIdx self,
                                                 std::string_view name) const {
    for (const CheckedModule& checked : modules) {
      for (const CheckedModule::MethodInfo& method : checked.methods) {
        if (method.self_type.idx == self.idx && method.name == name) {
          return &method;
        }
      }
    }
    return nullptr;
  }

  struct PathValue {
    enum class Kind : u8 {
      Local,
      Static,
      Function,
      AssocFunction,
      UnitVariant,
      TupleVariant,
      BlessedCtor,
      Type,
    };
    Kind kind = Kind::Type;
    ir::TypeIdx type = ir::TypeIdx(0);
    const CheckedModule::FnSig* function = nullptr;
    const CheckedModule::MethodInfo* method = nullptr;
    NominalEntry* enom = nullptr;
    u32 variant = 0;
    const BlessedEntry* blessed = nullptr;
    bool blessed_first = true;
    std::string_view ctor_name;
  };

  bool resolve_blessed_ctor(std::string_view name, PathValue& out) {
    const bool want_result = name == "Ok" || name == "Err";
    const bool want_option = name == "Some" || name == "None";
    if (!want_result && !want_option) {
      return false;
    }
    for (const BlessedEntry& entry : blessed) {
      if (entry.is_result == want_result) {
        out.kind = PathValue::Kind::BlessedCtor;
        out.blessed = &entry;
        out.blessed_first = (name == "Ok" || name == "Some");
        out.ctor_name = name;
        return true;
      }
    }
    return false;
  }

  // Resolves an expression path to its value meaning. Locals shadow
  // everything; nominal type names resolve to Kind::Type so callers
  // can report "found type" instead of "unknown".
  bool resolve_value_path(u32 module, const ast::Path* path, PathValue& out) {
    if (path->segments.empty()) {
      return false;
    }
    if (path->segments.size() == 1) {
      const std::string_view name = path->segments[0].name;
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
      VariantMatch match;
      if (find_variant(module, name, path->span, match)) {
        const ast::EnumItem* decl =
            static_cast<const ast::EnumItem*>(match.enom->item);
        out.enom = match.enom;
        out.variant = match.variant;
        out.type = intern_nominal(*match.enom);
        if (decl->variants[match.variant].fields.empty()) {
          out.kind = PathValue::Kind::UnitVariant;
        } else {
          out.kind = PathValue::Kind::TupleVariant;
        }
        return true;
      }
      if (resolve_blessed_ctor(name, out)) {
        return true;
      }
      if (find_nominal_in_scope(module, name) != nullptr) {
        out.kind = PathValue::Kind::Type;
        return true;
      }
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                                 path->span, "unresolved value '{}'", name);
      (void)index;
      return false;
    }
    if (path->segments.size() == 2) {
      const std::string_view head = path->segments[0].name;
      const std::string_view member = path->segments[1].name;
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
          const ast::EnumItem* decl =
              static_cast<const ast::EnumItem*>(matches[0].enom->item);
          out.enom = matches[0].enom;
          out.variant = matches[0].variant;
          out.type = intern_nominal(*matches[0].enom);
          out.kind = decl->variants[matches[0].variant].fields.empty()
                         ? PathValue::Kind::UnitVariant
                         : PathValue::Kind::TupleVariant;
          return true;
        }
        const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                                   path->span, "unresolved value '{}'", member);
        (void)index;
        return false;
      }
      // Nominal prefix: `Enum::Variant` or `Type::assoc`.
      NominalEntry* nominal = find_nominal_in_scope(module, head);
      if (nominal != nullptr && nominal->item->kind == ast::ItemKind::Enum) {
        const ast::EnumItem* decl =
            static_cast<const ast::EnumItem*>(nominal->item);
        for (u32 i = 0; i < static_cast<u32>(decl->variants.size()); ++i) {
          if (decl->variants[i].name.name == member) {
            out.enom = nominal;
            out.variant = i;
            out.type = intern_nominal(*nominal);
            out.kind = decl->variants[i].fields.empty()
                           ? PathValue::Kind::UnitVariant
                           : PathValue::Kind::TupleVariant;
            return true;
          }
        }
      }
      if (nominal != nullptr) {
        const ir::TypeIdx self = intern_nominal(*nominal);
        if (const CheckedModule::MethodInfo* method =
                lookup_method(self, member)) {
          if (method->receiver == CheckedModule::ReceiverKind::None) {
            out.kind = PathValue::Kind::AssocFunction;
            out.method = method;
            return true;
          }
        }
      }
      if ((head == "Result" || head == "Option") &&
          resolve_blessed_ctor(member, out)) {
        return true;
      }
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnknownValue, path->span,
                   "unresolved value '{}::{}'", head, member);
      (void)index;
      return false;
    }
    u32 target = kNoModule;
    if (!walk_module_prefix(module, path, "value", target)) {
      return false;
    }
    const std::string_view member = path->segments.back().name;
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
                               path->span, "unresolved value '{}'", member);
    (void)index;
    return false;
  }

  // Resolves a pattern/callee path to an enum variant: a bare name
  // in scope, `Enum::Variant`, `module::Variant`, or a blessed
  // constructor (`Ok`, `Err`, `Some`, `None`; the instantiation is
  // fixed later against the scrutinee type, so blessed stays null).
  bool resolve_variant_path(u32 module, const ast::Path* path, PathValue& out) {
    if (path->segments.empty()) {
      return false;
    }
    if (path->segments.size() == 1) {
      const std::string_view name = path->segments[0].name;
      VariantMatch match;
      if (find_variant(module, name, path->span, match)) {
        out.kind = PathValue::Kind::TupleVariant;
        out.enom = match.enom;
        out.variant = match.variant;
        out.type = intern_nominal(*match.enom);
        return true;
      }
      if (name == "Ok" || name == "Err" || name == "Some" || name == "None") {
        out.kind = PathValue::Kind::BlessedCtor;
        out.blessed = nullptr;
        out.blessed_first = (name == "Ok" || name == "Some");
        out.ctor_name = name;
        return true;
      }
      return false;
    }
    if (path->segments.size() == 2) {
      const std::string_view head = path->segments[0].name;
      const std::string_view member = path->segments[1].name;
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
          out.type = intern_nominal(*matches[0].enom);
          return true;
        }
        return false;
      }
      if (NominalEntry* nominal = find_nominal_in_scope(module, head)) {
        if (nominal->item->kind == ast::ItemKind::Enum) {
          const ast::EnumItem* decl =
              static_cast<const ast::EnumItem*>(nominal->item);
          for (u32 i = 0; i < static_cast<u32>(decl->variants.size()); ++i) {
            if (decl->variants[i].name.name == member) {
              out.kind = PathValue::Kind::TupleVariant;
              out.enom = nominal;
              out.variant = i;
              out.type = intern_nominal(*nominal);
              return true;
            }
          }
        }
      }
      if ((head == "Result" || head == "Option") &&
          (member == "Ok" || member == "Err" || member == "Some" ||
           member == "None")) {
        const bool want_result = head == "Result";
        const bool first = (member == "Ok" || member == "Some");
        if ((want_result && (member == "Ok" || member == "Err")) ||
            (!want_result && (member == "Some" || member == "None"))) {
          out.kind = PathValue::Kind::BlessedCtor;
          out.blessed = nullptr;
          out.blessed_first = first;
          out.ctor_name = member;
          return true;
        }
      }
      return false;
    }
    return false;
  }

  // Payload types of a resolved variant against a known enum type.
  std::vector<ir::TypeIdx> variant_payloads(const PathValue& resolved,
                                            ir::TypeIdx enum_type,
                                            diag::Span span) {
    if (is_error(enum_type)) {
      return {};
    }
    if (resolved.kind != PathValue::Kind::BlessedCtor) {
      const ast::EnumItem* decl =
          static_cast<const ast::EnumItem*>(resolved.enom->item);
      std::vector<ir::TypeIdx> payloads;
      payloads.reserve(decl->variants[resolved.variant].fields.size());
      for (const ast::Type* field : decl->variants[resolved.variant].fields) {
        payloads.push_back(resolve_type(resolved.enom->module, field, nullptr));
      }
      return payloads;
    }
    const BlessedEntry* entry = blessed_find(enum_type);
    if (entry == nullptr) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, span,
                   "blessed constructor on a non-blessed type");
      (void)index;
      return {};
    }
    if (resolved.blessed_first) {
      return {entry->args[0]};
    }
    if (entry->is_result) {
      return {entry->args[1]};
    }
    return {};
  }

  NominalEntry* resolve_struct_path(u32 module, const ast::Path* path) {
    if (path->segments.empty()) {
      return nullptr;
    }
    if (path->segments.size() == 1) {
      NominalEntry* nominal =
          find_nominal_in_scope(module, path->segments[0].name);
      if (nominal != nullptr && nominal->item->kind == ast::ItemKind::Struct) {
        return nominal;
      }
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnknownValue, path->span,
                   "unresolved struct '{}'", path->segments[0].name);
      (void)index;
      return nullptr;
    }
    u32 target = kNoModule;
    if (!walk_module_prefix(module, path, "value", target)) {
      return nullptr;
    }
    NominalEntry* nominal = find_nominal(target, path->segments.back().name);
    if (nominal != nullptr && nominal->item->kind == ast::ItemKind::Struct) {
      return nominal;
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerUnknownValue, path->span,
                 "unresolved struct '{}'", path->segments.back().name);
    (void)index;
    return nullptr;
  }

  // ---- Patterns ----

  void collect_pattern_idents(const ast::Pattern* pattern,
                              std::vector<std::string_view>& out) {
    switch (pattern->kind) {
      case ast::PatternKind::Wildcard:
      case ast::PatternKind::Literal: return;
      case ast::PatternKind::Ident: {
        const ast::IdentPattern* ident =
            static_cast<const ast::IdentPattern*>(pattern);
        out.push_back(ident->name.name);
        return;
      }
      case ast::PatternKind::MutIdent: {
        const ast::MutIdentPattern* ident =
            static_cast<const ast::MutIdentPattern*>(pattern);
        out.push_back(ident->name.name);
        return;
      }
      case ast::PatternKind::Tuple: {
        const ast::TuplePattern* tuple =
            static_cast<const ast::TuplePattern*>(pattern);
        for (const ast::Pattern* element : tuple->elements) {
          collect_pattern_idents(element, out);
        }
        return;
      }
      case ast::PatternKind::Struct: {
        const ast::StructPattern* strukt =
            static_cast<const ast::StructPattern*>(pattern);
        for (const ast::FieldPattern& field : strukt->fields) {
          collect_pattern_idents(field.pattern, out);
        }
        return;
      }
      case ast::PatternKind::Ref: {
        const ast::RefPattern* ref =
            static_cast<const ast::RefPattern*>(pattern);
        collect_pattern_idents(ref->inner, out);
        return;
      }
      case ast::PatternKind::Or: {
        const ast::OrPattern* or_pat =
            static_cast<const ast::OrPattern*>(pattern);
        for (const ast::Pattern* alt : or_pat->alternatives) {
          collect_pattern_idents(alt, out);
        }
        return;
      }
    }
  }

  void bind_error_idents(u32 module, const ast::Pattern* pattern) {
    std::vector<std::string_view> names;
    collect_pattern_idents(pattern, names);
    (void)module;
    for (std::string_view name : names) {
      scopes.back().push_back({name, error_type(), false});
    }
  }

  // Binds a pattern against a type, declaring locals. Returns true
  // when the pattern is refutable (declarations reject those).
  bool bind_pattern(u32 module, const ast::Pattern* pattern, ir::TypeIdx type) {
    switch (pattern->kind) {
      case ast::PatternKind::Wildcard: return false;
      case ast::PatternKind::Ident: {
        const ast::IdentPattern* ident =
            static_cast<const ast::IdentPattern*>(pattern);
        scopes.back().push_back({ident->name.name, type, false});
        return false;
      }
      case ast::PatternKind::MutIdent: {
        const ast::MutIdentPattern* ident =
            static_cast<const ast::MutIdentPattern*>(pattern);
        scopes.back().push_back({ident->name.name, type, true});
        return false;
      }
      case ast::PatternKind::Literal: {
        const ast::LiteralPattern* lit =
            static_cast<const ast::LiteralPattern*>(pattern);
        check_literal(lit->value, &type);
        return true;
      }
      case ast::PatternKind::Tuple: {
        const ast::TuplePattern* tuple =
            static_cast<const ast::TuplePattern*>(pattern);
        if (tuple->path != nullptr) {
          PathValue resolved;
          if (!resolve_variant_path(module, tuple->path, resolved)) {
            bind_error_idents(module, pattern);
            return true;
          }
          if (resolved.kind != PathValue::Kind::TupleVariant &&
              resolved.kind != PathValue::Kind::BlessedCtor) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                         pattern->span, "not a tuple variant");
            (void)index;
            bind_error_idents(module, pattern);
            return true;
          }
          if (resolved.kind == PathValue::Kind::TupleVariant) {
            unify(type, resolved.type, pattern->span, "tuple variant pattern");
          }
          std::vector<ir::TypeIdx> payloads =
              variant_payloads(resolved, type, pattern->span);
          if (payloads.size() != tuple->elements.size()) {
            const u32 index = bag.emit(
                diag::Severity::Error, kAnalyzerArityError, pattern->span,
                "variant expects {} fields, pattern has {}", payloads.size(),
                tuple->elements.size());
            (void)index;
            bind_error_idents(module, pattern);
            return true;
          }
          bool refutable = true;
          for (usize i = 0; i < payloads.size(); ++i) {
            refutable = bind_pattern(module, tuple->elements[i], payloads[i]) &&
                        refutable;
          }
          return refutable;
        }
        if (tag_of(type) != ir::TypeTag::Tuple) {
          unify(type, builder.tuple_type({ir::TypeIdx(0), 0}), pattern->span,
                "tuple pattern");
          bind_error_idents(module, pattern);
          return false;
        }
        const ir::TupleType& tuple_type =
            builder.tuple_types()[builder.types()[type].as_tuple()];
        if (tuple_type.elements.size() != tuple->elements.size()) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerArityError, pattern->span,
              "tuple pattern has {} elements, type has {}",
              tuple->elements.size(), tuple_type.elements.size());
          (void)index;
          bind_error_idents(module, pattern);
          return false;
        }
        bool refutable = false;
        for (u32 i = 0; i < static_cast<u32>(tuple->elements.size()); ++i) {
          if (bind_pattern(module, tuple->elements[i],
                           tuple_type.elements[i])) {
            refutable = true;
          }
        }
        return refutable;
      }
      case ast::PatternKind::Struct: {
        const ast::StructPattern* strukt =
            static_cast<const ast::StructPattern*>(pattern);
        NominalEntry* nominal = resolve_struct_path(module, strukt->path);
        if (nominal == nullptr) {
          bind_error_idents(module, pattern);
          return false;
        }
        unify(type, intern_nominal(*nominal), pattern->span, "struct pattern");
        const ast::StructItem* decl =
            static_cast<const ast::StructItem*>(nominal->item);
        const ir::StructType& struct_type =
            builder.struct_types()[builder.types()[nominal->type].as_struct()];
        bool refutable = false;
        for (const ast::FieldPattern& field : strukt->fields) {
          u32 index = 0;
          bool found = false;
          for (const ast::StructField& decl_field : decl->fields) {
            if (decl_field.name.name == field.name.name) {
              found = true;
              break;
            }
            ++index;
          }
          if (!found) {
            const u32 diag = bag.emit(diag::Severity::Error,
                                      kAnalyzerUnknownValue, field.name.span,
                                      "unknown field '{}'", field.name.name);
            (void)diag;
            bind_error_idents(module, field.pattern);
            continue;
          }
          if (bind_pattern(module, field.pattern, struct_type.fields[index])) {
            refutable = true;
          }
        }
        return refutable;
      }
      case ast::PatternKind::Ref: {
        const ast::RefPattern* ref =
            static_cast<const ast::RefPattern*>(pattern);
        const ir::TypeTag tag = tag_of(type);
        if ((ref->is_mut && tag != ir::TypeTag::MutRef) ||
            (!ref->is_mut && tag != ir::TypeTag::Ref)) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerTypeMismatch, pattern->span,
              "reference pattern on a non-reference type");
          (void)index;
          bind_error_idents(module, pattern);
          return false;
        }
        const ir::TypeIdx pointee =
            builder.ref_types()[builder.types()[type].as_ref()].pointee;
        return bind_pattern(module, ref->inner, pointee);
      }
      case ast::PatternKind::Or: {
        const ast::OrPattern* or_pat =
            static_cast<const ast::OrPattern*>(pattern);
        if (or_pat->alternatives.empty()) {
          return false;
        }
        bool refutable = bind_pattern(module, or_pat->alternatives[0], type);
        const usize base = scopes.back().size();
        for (usize i = 1; i < or_pat->alternatives.size(); ++i) {
          const usize mark = scopes.back().size();
          if (bind_pattern(module, or_pat->alternatives[i], type)) {
            refutable = true;
          }
          // Every alternative must bind the same names; extras are
          // dropped with a diagnostic.
          for (usize j = mark; j < scopes.back().size(); ++j) {
            bool found = false;
            for (usize k = base; k < mark; ++k) {
              if (scopes.back()[k].name == scopes.back()[j].name) {
                found = true;
                break;
              }
            }
            if (!found) {
              const u32 index =
                  bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                           or_pat->alternatives[i]->span,
                           "or-pattern alternatives must bind the same names");
              (void)index;
            }
          }
          scopes.back().erase(
              scopes.back().begin() +
                  static_cast<std::vector<Local>::difference_type>(mark),
              scopes.back().end());
        }
        return refutable;
      }
    }
  }

  // ---- Expressions ----

  bool is_bare_int_literal(const ast::Expr* expr) const {
    if (expr->kind != ast::ExprKind::Literal) {
      return false;
    }
    const ast::LiteralExpr* lit = static_cast<const ast::LiteralExpr*>(expr);
    if (lit->value->kind != ast::LiteralKind::Integer) {
      return false;
    }
    const std::string_view spelling = lit->value->spelling;
    for (char c : spelling) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return false;
      }
    }
    return true;
  }

  // Checks operands of a binary operator: same-type numerics, with
  // bare integer literals coerced to the other side (so `x + 42`
  // works for any integer x without an annotation).
  ir::TypeIdx check_binary_operands(u32 module,
                                    const ast::Expr* lhs,
                                    const ast::Expr* rhs,
                                    diag::Span span,
                                    std::string_view what) {
    ir::TypeIdx left = check_expr(module, lhs, nullptr);
    ir::TypeIdx right = check_expr(module, rhs, nullptr);
    if (!types_equal(left, right) && !is_error(left) && !is_error(right)) {
      if (is_integer_tag(tag_of(right)) && is_bare_int_literal(lhs)) {
        left = check_expr(module, lhs, &right);
      } else if (is_integer_tag(tag_of(left)) && is_bare_int_literal(rhs)) {
        right = check_expr(module, rhs, &left);
      }
    }
    if (is_error(left)) {
      return right;
    }
    if (is_error(right)) {
      return left;
    }
    if (types_equal(left, right)) {
      return left;
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, span,
                 "type mismatch in {}: '{}' vs '{}'", what,
                 ir::type_to_str(tag_of(left)), ir::type_to_str(tag_of(right)));
    (void)index;
    return error_type();
  }

  void check_call_args(u32 module,
                       std::span<ast::Expr* const> args,
                       const std::vector<ir::TypeIdx>& params,
                       diag::Span span,
                       std::string_view what,
                       bool skip_first) {
    const usize fixed = skip_first ? 1 : 0;
    if (args.size() + fixed != params.size()) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerArityError,
                                 span, "'{}' expects {} arguments, found {}",
                                 what, params.size() - fixed, args.size());
      (void)index;
      return;
    }
    for (usize i = 0; i < args.size(); ++i) {
      const ir::TypeIdx param = params[fixed + i];
      const ir::TypeIdx actual = check_expr(module, args[i], &param);
      unify(param, actual, args[i]->span, "argument");
    }
  }

  ir::TypeIdx check_path_expr(u32 module,
                              const ast::Path* path,
                              const ir::TypeIdx* expected,
                              diag::Span span) {
    PathValue resolved;
    if (!resolve_value_path(module, path, resolved)) {
      return error_type();
    }
    switch (resolved.kind) {
      case PathValue::Kind::Local:
      case PathValue::Kind::Static:
      case PathValue::Kind::UnitVariant: {
        if (expected != nullptr) {
          return unify(*expected, resolved.type, span, "path");
        }
        return resolved.type;
      }
      case PathValue::Kind::Function:
      case PathValue::Kind::AssocFunction:
      case PathValue::Kind::TupleVariant: {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                     "callee needs arguments");
        (void)index;
        return error_type();
      }
      case PathValue::Kind::BlessedCtor: {
        // Only `None` stands alone as a value; payload constructors
        // need call syntax. The expectation selects the instantiation.
        if (resolved.ctor_name != "None") {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                       "callee needs arguments");
          (void)index;
          return error_type();
        }
        if (expected != nullptr) {
          if (const BlessedEntry* entry = blessed_find(*expected)) {
            if (!entry->is_result) {
              return *expected;
            }
          }
        }
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                     "cannot infer the blessed type; add an annotation");
        (void)index;
        return error_type();
      }
      case PathValue::Kind::Type: {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                     "expected a value, found a type");
        (void)index;
        return error_type();
      }
    }
  }

  // Calls through a resolved callee path: free and associated
  // functions, tuple variant constructors (user and blessed), and
  // the `print` intrinsic.
  ir::TypeIdx check_call(u32 module,
                         const ast::Expr* callee,
                         std::span<ast::Expr* const> args,
                         const ir::TypeIdx* expected,
                         diag::Span span) {
    if (callee->kind != ast::ExprKind::Path) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                   callee->span, "cannot call a non-path expression");
      (void)index;
      for (const ast::Expr* arg : args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    const ast::PathExpr* path = static_cast<const ast::PathExpr*>(callee);
    if (path->path->segments.size() == 1 &&
        lookup_local(path->path->segments[0].name) == nullptr &&
        lookup_static(module, path->path->segments[0].name) == nullptr &&
        lookup_function(module, path->path->segments[0].name) == nullptr) {
      const std::string_view name = path->path->segments[0].name;
      if (name == "print") {
        if (args.size() != 1) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                       "'print' expects 1 argument, found {}", args.size());
          (void)index;
          return error_type();
        }
        const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
        const ir::TypeIdx actual = check_expr(module, args[0], &str);
        unify(str, actual, args[0]->span, "print argument");
        const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
        if (expected != nullptr) {
          return unify(*expected, unit, span, "call");
        }
        return unit;
      }
      if (name == "panic") {
        if (args.size() != 1) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                       "'panic' expects 1 argument, found {}", args.size());
          (void)index;
          return error_type();
        }
        const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
        const ir::TypeIdx actual = check_expr(module, args[0], &str);
        unify(str, actual, args[0]->span, "panic argument");
        return builder.never_type();
      }
    }
    PathValue resolved;
    if (!resolve_value_path(module, path->path, resolved)) {
      for (const ast::Expr* arg : args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    if (resolved.kind == PathValue::Kind::Function) {
      const CheckedModule::FnSig* fn = resolved.function;
      check_call_args(module, args, fn->params, span, fn->name, false);
      if (expected != nullptr) {
        return unify(*expected, fn->ret, span, "call");
      }
      return fn->ret;
    }
    if (resolved.kind == PathValue::Kind::AssocFunction) {
      const CheckedModule::MethodInfo* method = resolved.method;
      check_call_args(module, args, method->params, span, method->name, true);
      if (expected != nullptr) {
        return unify(*expected, method->ret, span, "call");
      }
      return method->ret;
    }
    if (resolved.kind == PathValue::Kind::TupleVariant ||
        resolved.kind == PathValue::Kind::BlessedCtor) {
      ir::TypeIdx enum_type = resolved.type;
      std::vector<ir::TypeIdx> payloads;
      if (resolved.kind == PathValue::Kind::BlessedCtor) {
        // The annotation (or other expectation) selects the
        // instantiation; any already-interned same-kind entry is only
        // a fallback for inference-free positions.
        const BlessedEntry* entry = nullptr;
        if (expected != nullptr) {
          entry = blessed_find(*expected);
        }
        if (entry == nullptr) {
          entry = resolved.blessed;
        }
        if (entry == nullptr) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                       "cannot infer the blessed type; add an annotation");
          (void)index;
          for (const ast::Expr* arg : args) {
            check_expr(module, arg, nullptr);
          }
          return error_type();
        }
        const bool wants_result =
            resolved.ctor_name == "Ok" || resolved.ctor_name == "Err";
        if (wants_result != entry->is_result) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, span,
                       "'{}' is not a variant of '{}'", resolved.ctor_name,
                       entry->is_result ? "Option" : "Result");
          (void)index;
          return error_type();
        }
        enum_type = entry->type;
        payloads = variant_payloads(resolved, enum_type, span);
      } else {
        payloads = variant_payloads(resolved, enum_type, span);
      }
      if (args.size() != payloads.size()) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                     "variant expects {} arguments, found {}", payloads.size(),
                     args.size());
        (void)index;
        for (const ast::Expr* arg : args) {
          check_expr(module, arg, nullptr);
        }
        return error_type();
      }
      for (usize i = 0; i < args.size(); ++i) {
        const ir::TypeIdx actual = check_expr(module, args[i], &payloads[i]);
        unify(payloads[i], actual, args[i]->span, "variant argument");
      }
      if (expected != nullptr) {
        return unify(*expected, enum_type, span, "call");
      }
      return enum_type;
    }
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                               span, "not callable");
    (void)index;
    for (const ast::Expr* arg : args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }

  ir::TypeIdx check_method_call(u32 module,
                                const ast::MethodCallExpr* call,
                                const ir::TypeIdx* expected) {
    const ir::TypeIdx receiver = check_expr(module, call->receiver, nullptr);
    if (is_error(receiver)) {
      for (const ast::Expr* arg : call->args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    const std::string_view name = call->name.name;
    if (const BlessedEntry* entry = blessed_find(receiver)) {
      const ir::TypeIdx ok = entry->args[0];
      if (name == "unwrap") {
        if (!call->args.empty()) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerArityError, call->span,
              "'unwrap' expects 0 arguments, found {}", call->args.size());
          (void)index;
          return error_type();
        }
        if (expected != nullptr) {
          return unify(*expected, ok, call->span, "method call");
        }
        return ok;
      }
      if (name == "expect") {
        if (call->args.size() != 1) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerArityError, call->span,
              "'expect' expects 1 argument, found {}", call->args.size());
          (void)index;
          return error_type();
        }
        const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
        const ir::TypeIdx actual = check_expr(module, call->args[0], &str);
        unify(str, actual, call->args[0]->span, "expect argument");
        if (expected != nullptr) {
          return unify(*expected, ok, call->span, "method call");
        }
        return ok;
      }
      if (name == "is_ok" || name == "is_err") {
        if (!call->args.empty()) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerArityError, call->span,
              "'{}' expects 0 arguments, found {}", name, call->args.size());
          (void)index;
          return error_type();
        }
        const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
        if (expected != nullptr) {
          return unify(*expected, boolean, call->span, "method call");
        }
        return boolean;
      }
    }
    ir::TypeIdx nominal = receiver;
    const ir::TypeTag tag = tag_of(receiver);
    if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
      nominal = builder.ref_types()[builder.types()[receiver].as_ref()].pointee;
    }
    const CheckedModule::MethodInfo* method = lookup_method(nominal, name);
    if (method == nullptr) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                                 call->name.span, "no method '{}'", name);
      (void)index;
      for (const ast::Expr* arg : call->args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    if (method->receiver == CheckedModule::ReceiverKind::None) {
      const u32 index = bag.emit(
          diag::Severity::Error, kAnalyzerInvalidOperation, call->name.span,
          "associated function '{}' called as a method", name);
      (void)index;
      return error_type();
    }
    // No autoref/deref in MVP beyond this: an owned receiver coerces
    // to the declared borrow; borrow checking itself is Phase C.
    const ir::TypeIdx declared = method->params[0];
    if (receiver.idx != declared.idx) {
      bool coerced = false;
      if (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef) {
        const ir::TypeTag declared_tag = tag_of(declared);
        if ((declared_tag == ir::TypeTag::Ref ||
             declared_tag == ir::TypeTag::MutRef) &&
            builder.ref_types()[builder.types()[declared].as_ref()]
                    .pointee.idx == receiver.idx) {
          coerced = true;
        }
      }
      if (!coerced) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                     call->receiver->span, "type mismatch in receiver");
        (void)index;
        return error_type();
      }
    }
    std::vector<ir::TypeIdx> rest(method->params.begin() + 1,
                                  method->params.end());
    check_call_args(module, call->args, rest, call->span, name, false);
    if (expected != nullptr) {
      return unify(*expected, method->ret, call->span, "method call");
    }
    return method->ret;
  }

  ir::TypeIdx check_field(u32 module,
                          const ast::FieldExpr* field,
                          const ir::TypeIdx* expected) {
    ir::TypeIdx receiver = check_expr(module, field->receiver, nullptr);
    if (is_error(receiver)) {
      return error_type();
    }
    // Field access sees through references (method receivers are
    // commonly `&Self`); borrow checking itself is Phase C.
    while (tag_of(receiver) == ir::TypeTag::Ref ||
           tag_of(receiver) == ir::TypeTag::MutRef) {
      receiver =
          builder.ref_types()[builder.types()[receiver].as_ref()].pointee;
    }
    const ir::TypeTag tag = tag_of(receiver);
    if (tag == ir::TypeTag::Struct) {
      const ir::StructType& struct_type =
          builder.struct_types()[builder.types()[receiver].as_struct()];
      NominalEntry* owner = nullptr;
      u32 field_index = 0;
      for (NominalEntry& entry : nominals) {
        if (!entry.complete || entry.type.idx != receiver.idx ||
            entry.item->kind != ast::ItemKind::Struct) {
          continue;
        }
        const ast::StructItem* decl =
            static_cast<const ast::StructItem*>(entry.item);
        u32 i = 0;
        for (const ast::StructField& decl_field : decl->fields) {
          if (decl_field.name.name == field->name.name) {
            owner = &entry;
            field_index = i;
            break;
          }
          ++i;
        }
        if (owner != nullptr) {
          break;
        }
      }
      if (owner == nullptr) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                     field->name.span, "unknown field '{}'", field->name.name);
        (void)index;
        return error_type();
      }
      const ir::TypeIdx result = struct_type.fields[field_index];
      (void)module;
      if (expected != nullptr) {
        return unify(*expected, result, field->span, "field");
      }
      return result;
    }
    if (tag == ir::TypeTag::Tuple) {
      const ir::TupleType& tuple_type =
          builder.tuple_types()[builder.types()[receiver].as_tuple()];
      u32 index = 0;
      bool digits = !field->name.name.empty();
      for (char c : field->name.name) {
        if (c < '0' || c > '9') {
          digits = false;
          break;
        }
        index = index * 10 + static_cast<u32>(c - '0');
      }
      if (!digits || index >= tuple_type.elements.size()) {
        const u32 diag = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                                  field->name.span, "unknown tuple field '{}'",
                                  field->name.name);
        (void)diag;
        return error_type();
      }
      const ir::TypeIdx result = tuple_type.elements[index];
      if (expected != nullptr) {
        return unify(*expected, result, field->span, "field");
      }
      return result;
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, field->span,
                 "no fields on '{}'", ir::type_to_str(tag));
    (void)index;
    return error_type();
  }

  ir::TypeIdx check_struct_expr(u32 module,
                                const ast::StructExpr* strukt,
                                const ir::TypeIdx* expected) {
    NominalEntry* nominal = resolve_struct_path(module, strukt->path);
    if (nominal == nullptr) {
      for (const ast::FieldInit& field : strukt->init) {
        check_expr(module, field.value, nullptr);
      }
      if (strukt->base_expr != nullptr) {
        check_expr(module, strukt->base_expr, nullptr);
      }
      return error_type();
    }
    const ir::TypeIdx struct_type = intern_nominal(*nominal);
    const ast::StructItem* decl =
        static_cast<const ast::StructItem*>(nominal->item);
    const ir::StructType& fields =
        builder.struct_types()[builder.types()[struct_type].as_struct()];
    std::vector<bool> seen(decl->fields.size(), false);
    for (const ast::FieldInit& field : strukt->init) {
      u32 index = 0;
      bool found = false;
      for (const ast::StructField& decl_field : decl->fields) {
        if (decl_field.name.name == field.name.name) {
          found = true;
          break;
        }
        ++index;
      }
      if (!found) {
        const u32 diag =
            bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                     field.name.span, "unknown field '{}'", field.name.name);
        (void)diag;
        check_expr(module, field.value, nullptr);
        continue;
      }
      seen[index] = true;
      const ir::TypeIdx field_type = fields.fields[index];
      const ir::TypeIdx actual = check_expr(module, field.value, &field_type);
      unify(field_type, actual, field.value->span, "field");
    }
    if (strukt->base_expr != nullptr) {
      const ir::TypeIdx base = check_expr(module, strukt->base_expr, nullptr);
      unify(struct_type, base, strukt->base_expr->span, "struct update base");
    } else {
      for (usize i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
          const u32 diag =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, strukt->span,
                       "missing field '{}'", decl->fields[i].name.name);
          (void)diag;
        }
      }
    }
    if (expected != nullptr) {
      return unify(*expected, struct_type, strukt->span, "struct");
    }
    return struct_type;
  }

  ir::TypeIdx check_question(u32 module,
                             const ast::QuestionExpr* question,
                             const ir::TypeIdx* expected) {
    const ir::TypeIdx inner = check_expr(module, question->inner, nullptr);
    const BlessedEntry* scrutinee = blessed_find(inner);
    if (scrutinee == nullptr) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadQuestion,
                                 question->span, "'?' needs Result or Option");
      (void)index;
      return error_type();
    }
    const BlessedEntry* enclosing = blessed_find(fn_ret);
    if (enclosing == nullptr) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerBadQuestion, question->span,
                   "'?' needs an enclosing Result or Option function");
      (void)index;
      return error_type();
    }
    if (scrutinee->is_result != enclosing->is_result ||
        scrutinee->args.size() != enclosing->args.size()) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerBadQuestion, question->span,
                   "'?' type does not match the function return type");
      (void)index;
      return error_type();
    }
    for (usize i = 0; i < scrutinee->args.size(); ++i) {
      if (!types_equal(scrutinee->args[i], enclosing->args[i])) {
        const u32 index = bag.emit(
            diag::Severity::Error, kAnalyzerBadQuestion, question->span,
            "'?' type does not match the function return type");
        (void)index;
        return error_type();
      }
    }
    if (expected != nullptr) {
      return unify(*expected, scrutinee->args[0], question->span, "'?'");
    }
    return scrutinee->args[0];
  }

  ir::TypeIdx check_cast(u32 module,
                         const ast::CastExpr* cast,
                         const ir::TypeIdx* expected) {
    const ir::TypeIdx inner = check_expr(module, cast->inner, nullptr);
    const ir::TypeIdx target = resolve_type(module, cast->type, nullptr);
    if (is_error(inner) || is_error(target)) {
      return error_type();
    }
    const ir::TypeTag from = tag_of(inner);
    const ir::TypeTag to = tag_of(target);
    const bool numeric_from =
        is_integer_tag(from) || is_float_tag(from) || from == ir::TypeTag::I1;
    const bool numeric_to =
        is_integer_tag(to) || is_float_tag(to) || to == ir::TypeTag::I1;
    if (from == ir::TypeTag::Never || (numeric_from && numeric_to)) {
      if (expected != nullptr) {
        return unify(*expected, target, cast->span, "cast");
      }
      return target;
    }
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                               cast->span, "invalid cast from '{}' to '{}'",
                               ir::type_to_str(from), ir::type_to_str(to));
    (void)index;
    return error_type();
  }

  ir::TypeIdx check_index(u32 module,
                          const ast::IndexExpr* index,
                          const ir::TypeIdx* expected) {
    const ir::TypeIdx receiver = check_expr(module, index->receiver, nullptr);
    const ir::TypeIdx position = check_expr(module, index->index, nullptr);
    if (is_error(receiver) || is_error(position)) {
      return error_type();
    }
    if (!is_integer_tag(tag_of(position))) {
      const u32 diag =
          bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                   index->index->span, "array index must be an integer");
      (void)diag;
      return error_type();
    }
    if (tag_of(receiver) != ir::TypeTag::Array) {
      const u32 diag = bag.emit(
          diag::Severity::Error, kAnalyzerInvalidOperation, index->span,
          "cannot index '{}'", ir::type_to_str(tag_of(receiver)));
      (void)diag;
      return error_type();
    }
    const ir::ArrayType& array =
        builder.array_types()[builder.types()[receiver].as_array()];
    if (expected != nullptr) {
      return unify(*expected, array.element, index->span, "index");
    }
    return array.element;
  }

  void check_cond(u32 module, const ast::Cond* cond, bool& binds) {
    binds = false;
    if (cond->is_pattern) {
      const ir::TypeIdx init = check_expr(module, cond->init, nullptr);
      scopes.emplace_back();
      binds = true;
      bind_pattern(module, cond->pattern, init);
      return;
    }
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    const ir::TypeIdx actual = check_expr(module, cond->value, &boolean);
    unify(boolean, actual, cond->value->span, "condition");
  }

  ir::TypeIdx check_if(u32 module,
                       const ast::IfExpr* if_expr,
                       const ir::TypeIdx* expected) {
    bool binds = false;
    check_cond(module, if_expr->cond, binds);
    const ir::TypeIdx then = check_block(module, if_expr->then_block, expected);
    if (binds) {
      scopes.pop_back();
    }
    if (if_expr->else_block == nullptr) {
      if (!is_void(then) && !is_error(then) && !is_never(then)) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                     if_expr->span, "if without else yields '()'");
        (void)index;
      }
      const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
      if (expected != nullptr) {
        return unify(*expected, unit, if_expr->span, "if");
      }
      return unit;
    }
    const ir::TypeIdx otherwise =
        check_block(module, if_expr->else_block, expected);
    return unify(then, otherwise, if_expr->span, "if branches");
  }

  // Exhaustiveness over the plan's bounded scope: bool and enum
  // variants by enumeration, integer matches by wildcard (literal
  // ranges cannot cover a full integer type), tuples and structs by
  // wildcard or a matching constructor pattern.
  void check_exhaustive(ir::TypeIdx scrutinee,
                        std::span<const ast::MatchArm> arms,
                        diag::Span span) {
    bool wildcard = false;
    std::vector<bool> covered_bool{false, false};
    for (const ast::MatchArm& arm : arms) {
      if (pattern_is_wildcard(arm.pattern)) {
        wildcard = true;
      }
      collect_bool_literals(arm.pattern, covered_bool);
    }
    if (wildcard) {
      return;
    }
    const ir::TypeTag tag = tag_of(scrutinee);
    if (tag == ir::TypeTag::I1) {
      if (!covered_bool[0] || !covered_bool[1]) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                     "non-exhaustive match: missing '{}'",
                     !covered_bool[0] ? "true" : "false");
        (void)index;
      }
      return;
    }
    if (tag == ir::TypeTag::Enum) {
      const ir::EnumType& enum_type =
          builder.enum_types()[builder.types()[scrutinee].as_enum()];
      // Find the declaring nominal for variant names.
      const ast::EnumItem* decl = nullptr;
      for (NominalEntry& entry : nominals) {
        if (entry.complete && entry.type.idx == scrutinee.idx &&
            entry.item->kind == ast::ItemKind::Enum) {
          decl = static_cast<const ast::EnumItem*>(entry.item);
          break;
        }
      }
      if (decl == nullptr) {
        for (const BlessedEntry& entry : blessed) {
          if (entry.type.idx != scrutinee.idx) {
            continue;
          }
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                       "non-exhaustive match over '{}'",
                       entry.is_result ? "Result" : "Option");
          (void)index;
          return;
        }
        return;
      }
      std::vector<bool> covered(static_cast<usize>(enum_type.variants.size()),
                                false);
      for (const ast::MatchArm& arm : arms) {
        mark_variant_covered(arm.pattern, decl, covered);
      }
      for (usize i = 0; i < covered.size(); ++i) {
        if (!covered[i]) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                       "non-exhaustive match: '{}' not covered",
                       decl->variants[i].name.name);
          (void)index;
          return;
        }
      }
      return;
    }
    if (is_integer_tag(tag)) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                   "non-exhaustive integer match: add a wildcard arm");
      (void)index;
      return;
    }
    if (tag == ir::TypeTag::Tuple) {
      for (const ast::MatchArm& arm : arms) {
        if (arm.pattern->kind == ast::PatternKind::Tuple &&
            static_cast<const ast::TuplePattern*>(arm.pattern)->path ==
                nullptr) {
          return;
        }
      }
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                   "non-exhaustive tuple match: add a wildcard arm");
      (void)index;
      return;
    }
    if (tag == ir::TypeTag::Struct) {
      for (const ast::MatchArm& arm : arms) {
        if (arm.pattern->kind == ast::PatternKind::Struct) {
          return;
        }
      }
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                   "non-exhaustive struct match: add a wildcard arm");
      (void)index;
      return;
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                 "non-exhaustive match: add a wildcard arm");
    (void)index;
  }

  bool pattern_is_wildcard(const ast::Pattern* pattern) const {
    switch (pattern->kind) {
      case ast::PatternKind::Wildcard: return true;
      case ast::PatternKind::Ident:
      case ast::PatternKind::MutIdent: return true;
      case ast::PatternKind::Or: {
        const ast::OrPattern* or_pat =
            static_cast<const ast::OrPattern*>(pattern);
        for (const ast::Pattern* alt : or_pat->alternatives) {
          if (pattern_is_wildcard(alt)) {
            return true;
          }
        }
        return false;
      }
      default: return false;
    }
  }

  void collect_bool_literals(const ast::Pattern* pattern,
                             std::vector<bool>& covered) const {
    switch (pattern->kind) {
      case ast::PatternKind::Literal: {
        const ast::LiteralPattern* lit =
            static_cast<const ast::LiteralPattern*>(pattern);
        if (lit->value->kind == ast::LiteralKind::Bool) {
          covered[lit->value->spelling == "true" ? 0 : 1] = true;
        }
        return;
      }
      case ast::PatternKind::Or: {
        const ast::OrPattern* or_pat =
            static_cast<const ast::OrPattern*>(pattern);
        for (const ast::Pattern* alt : or_pat->alternatives) {
          collect_bool_literals(alt, covered);
        }
        return;
      }
      default: return;
    }
  }

  void mark_variant_covered(const ast::Pattern* pattern,
                            const ast::EnumItem* decl,
                            std::vector<bool>& covered) {
    switch (pattern->kind) {
      case ast::PatternKind::Ident: {
        const ast::IdentPattern* ident =
            static_cast<const ast::IdentPattern*>(pattern);
        for (usize i = 0; i < covered.size(); ++i) {
          if (decl->variants[i].name.name == ident->name.name &&
              decl->variants[i].fields.empty()) {
            covered[i] = true;
            return;
          }
        }
        return;
      }
      case ast::PatternKind::Tuple: {
        const ast::TuplePattern* tuple =
            static_cast<const ast::TuplePattern*>(pattern);
        if (tuple->path == nullptr || tuple->path->segments.empty()) {
          return;
        }
        const std::string_view name = tuple->path->segments.back().name;
        for (usize i = 0; i < covered.size(); ++i) {
          if (decl->variants[i].name.name == name) {
            covered[i] = true;
            return;
          }
        }
        return;
      }
      case ast::PatternKind::Or: {
        const ast::OrPattern* or_pat =
            static_cast<const ast::OrPattern*>(pattern);
        for (const ast::Pattern* alt : or_pat->alternatives) {
          mark_variant_covered(alt, decl, covered);
        }
        return;
      }
      default: return;
    }
  }

  ir::TypeIdx check_match(u32 module,
                          const ast::MatchExpr* match,
                          const ir::TypeIdx* expected) {
    const ir::TypeIdx scrutinee = check_expr(module, match->scrutinee, nullptr);
    ir::TypeIdx result = error_type();
    bool first = true;
    for (const ast::MatchArm& arm : match->arms) {
      scopes.emplace_back();
      if (!is_error(scrutinee)) {
        bind_pattern(module, arm.pattern, scrutinee);
      }
      const ir::TypeIdx body = check_expr(module, arm.body, expected);
      scopes.pop_back();
      if (first) {
        result = body;
        first = false;
      } else {
        result = unify(result, body, arm.body->span, "match arms");
      }
    }
    if (!is_error(scrutinee)) {
      check_exhaustive(scrutinee, match->arms, match->span);
    }
    if (expected != nullptr && !first) {
      return unify(*expected, result, match->span, "match");
    }
    return result;
  }

  ir::TypeIdx check_expr(u32 module,
                         const ast::Expr* expr,
                         const ir::TypeIdx* expected) {
    switch (expr->kind) {
      case ast::ExprKind::Literal: {
        const ast::LiteralExpr* lit =
            static_cast<const ast::LiteralExpr*>(expr);
        return check_literal(lit->value, expected);
      }
      case ast::ExprKind::Path: {
        const ast::PathExpr* path = static_cast<const ast::PathExpr*>(expr);
        return check_path_expr(module, path->path, expected, expr->span);
      }
      case ast::ExprKind::Struct: {
        const ast::StructExpr* strukt =
            static_cast<const ast::StructExpr*>(expr);
        return check_struct_expr(module, strukt, expected);
      }
      case ast::ExprKind::Tuple: {
        const ast::TupleExpr* tuple = static_cast<const ast::TupleExpr*>(expr);
        if (tuple->elements.empty()) {
          const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
          if (expected != nullptr) {
            return unify(*expected, unit, expr->span, "unit");
          }
          return unit;
        }
        const ir::TupleType* expected_tuple = nullptr;
        ir::TupleType expected_copy;
        if (expected != nullptr && tag_of(*expected) == ir::TypeTag::Tuple) {
          expected_copy =
              builder.tuple_types()[builder.types()[*expected].as_tuple()];
          expected_tuple = &expected_copy;
        }
        std::vector<ir::TypeIdx> elements;
        elements.reserve(tuple->elements.size());
        for (usize i = 0; i < tuple->elements.size(); ++i) {
          const ir::TypeIdx* element_expected = nullptr;
          ir::TypeIdx element_type = error_type();
          if (expected_tuple != nullptr &&
              expected_tuple->elements.size() == tuple->elements.size()) {
            element_type = expected_tuple->elements[i];
            element_expected = &element_type;
          }
          elements.push_back(
              check_expr(module, tuple->elements[i], element_expected));
        }
        ir::TypeSeq seq;
        for (ir::TypeIdx element : elements) {
          seq.push(builder.ref_type(element));
        }
        const ir::TypeIdx type = builder.tuple_type(seq.finish());
        if (expected != nullptr) {
          return unify(*expected, type, expr->span, "tuple");
        }
        return type;
      }
      case ast::ExprKind::Unary: {
        const ast::UnaryExpr* unary = static_cast<const ast::UnaryExpr*>(expr);
        const ir::TypeIdx inner = check_expr(module, unary->inner, nullptr);
        if (is_error(inner)) {
          return error_type();
        }
        const ir::TypeTag tag = tag_of(inner);
        switch (unary->op) {
          case ast::UnaryOp::Neg:
            if (is_integer_tag(tag) || is_float_tag(tag)) {
              if (expected != nullptr) {
                return unify(*expected, inner, expr->span, "negation");
              }
              return inner;
            }
            break;
          case ast::UnaryOp::Not:
            if (tag == ir::TypeTag::I1) {
              if (expected != nullptr) {
                return unify(*expected, inner, expr->span, "not");
              }
              return inner;
            }
            break;
          case ast::UnaryOp::BitNot:
            if (is_integer_tag(tag)) {
              if (expected != nullptr) {
                return unify(*expected, inner, expr->span, "bitwise not");
              }
              return inner;
            }
            break;
        }
        const u32 index = bag.emit(
            diag::Severity::Error, kAnalyzerInvalidOperation, expr->span,
            "invalid unary operand '{}'", ir::type_to_str(tag));
        (void)index;
        return error_type();
      }
      case ast::ExprKind::Binary: {
        const ast::BinaryExpr* binary =
            static_cast<const ast::BinaryExpr*>(expr);
        if (binary->op == ast::BinaryOp::And ||
            binary->op == ast::BinaryOp::Or) {
          const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
          const ir::TypeIdx left = check_expr(module, binary->lhs, &boolean);
          const ir::TypeIdx right = check_expr(module, binary->rhs, &boolean);
          unify(boolean, left, binary->lhs->span, "logical operand");
          unify(boolean, right, binary->rhs->span, "logical operand");
          if (expected != nullptr) {
            return unify(*expected, boolean, expr->span, "logical");
          }
          return boolean;
        }
        const ir::TypeIdx operands = check_binary_operands(
            module, binary->lhs, binary->rhs, expr->span, "binary");
        if (is_error(operands)) {
          return error_type();
        }
        const ir::TypeTag tag = tag_of(operands);
        switch (binary->op) {
          case ast::BinaryOp::Eq:
          case ast::BinaryOp::NotEq: {
            const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
            if (expected != nullptr) {
              return unify(*expected, boolean, expr->span, "comparison");
            }
            return boolean;
          }
          case ast::BinaryOp::Gt:
          case ast::BinaryOp::Lt:
          case ast::BinaryOp::GtEq:
          case ast::BinaryOp::LtEq:
            if (is_integer_tag(tag) || is_float_tag(tag)) {
              const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
              if (expected != nullptr) {
                return unify(*expected, boolean, expr->span, "comparison");
              }
              return boolean;
            }
            break;
          default:
            if (is_integer_tag(tag) || is_float_tag(tag)) {
              if (expected != nullptr) {
                return unify(*expected, operands, expr->span, "arithmetic");
              }
              return operands;
            }
            break;
        }
        const u32 index = bag.emit(
            diag::Severity::Error, kAnalyzerInvalidOperation, expr->span,
            "invalid binary operand '{}'", ir::type_to_str(tag));
        (void)index;
        return error_type();
      }
      case ast::ExprKind::Cast: {
        const ast::CastExpr* cast = static_cast<const ast::CastExpr*>(expr);
        return check_cast(module, cast, expected);
      }
      case ast::ExprKind::Call: {
        const ast::CallExpr* call = static_cast<const ast::CallExpr*>(expr);
        return check_call(module, call->callee, call->args, expected,
                          expr->span);
      }
      case ast::ExprKind::MethodCall: {
        const ast::MethodCallExpr* call =
            static_cast<const ast::MethodCallExpr*>(expr);
        return check_method_call(module, call, expected);
      }
      case ast::ExprKind::Field: {
        const ast::FieldExpr* field = static_cast<const ast::FieldExpr*>(expr);
        return check_field(module, field, expected);
      }
      case ast::ExprKind::Index: {
        const ast::IndexExpr* index = static_cast<const ast::IndexExpr*>(expr);
        return check_index(module, index, expected);
      }
      case ast::ExprKind::Question: {
        const ast::QuestionExpr* question =
            static_cast<const ast::QuestionExpr*>(expr);
        return check_question(module, question, expected);
      }
      case ast::ExprKind::If: {
        const ast::IfExpr* if_expr = static_cast<const ast::IfExpr*>(expr);
        return check_if(module, if_expr, expected);
      }
      case ast::ExprKind::Match: {
        const ast::MatchExpr* match = static_cast<const ast::MatchExpr*>(expr);
        return check_match(module, match, expected);
      }
      case ast::ExprKind::Loop: {
        const ast::LoopExpr* loop = static_cast<const ast::LoopExpr*>(expr);
        const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
        ++loop_depth;
        check_block(module, loop->body, &unit);
        --loop_depth;
        if (expected != nullptr) {
          return unify(*expected, unit, expr->span, "loop");
        }
        return unit;
      }
      case ast::ExprKind::While: {
        const ast::WhileExpr* while_expr =
            static_cast<const ast::WhileExpr*>(expr);
        bool binds = false;
        check_cond(module, while_expr->cond, binds);
        const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
        ++loop_depth;
        check_block(module, while_expr->body, &unit);
        --loop_depth;
        if (binds) {
          scopes.pop_back();
        }
        if (expected != nullptr) {
          return unify(*expected, unit, expr->span, "while");
        }
        return unit;
      }
      case ast::ExprKind::Block: {
        const ast::BlockExpr* block = static_cast<const ast::BlockExpr*>(expr);
        return check_block(module, block->block, expected);
      }
      case ast::ExprKind::Return: {
        const ast::ReturnExpr* ret = static_cast<const ast::ReturnExpr*>(expr);
        if (!in_fn) {
          const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadReturn,
                                     expr->span, "'ret' outside of a function");
          (void)index;
          return error_type();
        }
        if (ret->value == nullptr) {
          unify(fn_ret, builder.primitive(ir::TypeTag::Void), expr->span,
                "return");
        } else {
          const ir::TypeIdx value = check_expr(module, ret->value, &fn_ret);
          unify(fn_ret, value, ret->value->span, "return");
        }
        return builder.never_type();
      }
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue: {
        if (loop_depth == 0) {
          if (expr->kind == ast::ExprKind::Break) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerBreakOutsideLoop,
                         expr->span, "'break' outside of a loop");
            (void)index;
          } else {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerBreakOutsideLoop,
                         expr->span, "'continue' outside of a loop");
            (void)index;
          }
          return error_type();
        }
        return builder.never_type();
      }
      case ast::ExprKind::Range: {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnsupportedExpr,
                     expr->span, "range expressions arrive post-MVP");
        (void)index;
        return error_type();
      }
    }
  }

  ir::TypeIdx check_block(u32 module,
                          const ast::Block* block,
                          const ir::TypeIdx* expected) {
    scopes.emplace_back();
    for (ast::Stmt* stmt : block->statements) {
      check_stmt(module, stmt);
    }
    ir::TypeIdx result = builder.primitive(ir::TypeTag::Void);
    if (block->value != nullptr) {
      result = check_expr(module, block->value, expected);
      if (expected != nullptr) {
        result = unify(*expected, result, block->value->span, "block");
      }
    } else if (expected != nullptr) {
      result = unify(*expected, result, block->span, "block");
    }
    scopes.pop_back();
    return result;
  }

  ir::TypeIdx check_place(u32 module, const ast::Expr* place) {
    switch (place->kind) {
      case ast::ExprKind::Path: {
        const ast::PathExpr* path = static_cast<const ast::PathExpr*>(place);
        PathValue resolved;
        if (!resolve_value_path(module, path->path, resolved)) {
          return error_type();
        }
        if (resolved.kind != PathValue::Kind::Local) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerBadAssignment,
                       place->span, "cannot assign to this place");
          (void)index;
          return error_type();
        }
        // Locals shadow everything, but the resolution above may have
        // found the name through another namespace; confirm mutability
        // through the scope entry.
        const Local* local = lookup_local(path->path->segments.back().name);
        if (local == nullptr || !local->is_mut) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerBadAssignment,
                       place->span, "cannot assign to an immutable binding");
          (void)index;
          return error_type();
        }
        return local->type;
      }
      case ast::ExprKind::Field: {
        const ast::FieldExpr* field = static_cast<const ast::FieldExpr*>(place);
        const ir::TypeIdx receiver = check_place(module, field->receiver);
        if (is_error(receiver)) {
          return error_type();
        }
        if (tag_of(receiver) != ir::TypeTag::Struct) {
          return error_type();
        }
        return check_field(module, field, nullptr);
      }
      case ast::ExprKind::Index: {
        const ast::IndexExpr* index = static_cast<const ast::IndexExpr*>(place);
        const ir::TypeIdx receiver = check_place(module, index->receiver);
        if (is_error(receiver)) {
          return error_type();
        }
        return check_index(module, index, nullptr);
      }
      default: {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerBadAssignment, place->span,
                     "cannot assign to this place");
        (void)index;
        return error_type();
      }
    }
  }

  void check_stmt(u32 module, const ast::Stmt* stmt) {
    switch (stmt->kind) {
      case ast::StmtKind::Decl: {
        const ast::DeclStmt* decl = static_cast<const ast::DeclStmt*>(stmt);
        const ir::TypeIdx* expected = nullptr;
        ir::TypeIdx ascribed = error_type();
        if (decl->type != nullptr) {
          ascribed = resolve_type(module, decl->type, nullptr);
          expected = &ascribed;
        }
        const ir::TypeIdx init = check_expr(module, decl->init, expected);
        if (expected != nullptr) {
          unify(*expected, init, decl->init->span, "declaration");
        }
        if (bind_pattern(module, decl->pattern, init)) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerRefutableLet, decl->pattern->span,
              "refutable pattern in declaration; use match");
          (void)index;
        }
        return;
      }
      case ast::StmtKind::Reassign: {
        const ast::ReassignStmt* reassign =
            static_cast<const ast::ReassignStmt*>(stmt);
        const ir::TypeIdx place = check_place(module, reassign->place);
        const ir::TypeIdx value = check_expr(module, reassign->value, &place);
        if (reassign->compound) {
          const ir::TypeTag tag = tag_of(place);
          if (!is_integer_tag(tag) && !is_float_tag(tag)) {
            const u32 index = bag.emit(
                diag::Severity::Error, kAnalyzerInvalidOperation, stmt->span,
                "compound assignment needs a numeric place");
            (void)index;
            return;
          }
        }
        unify(place, value, reassign->value->span, "assignment");
        return;
      }
      case ast::StmtKind::Expr: {
        const ast::ExprStmt* expr_stmt =
            static_cast<const ast::ExprStmt*>(stmt);
        const ir::TypeIdx type = check_expr(module, expr_stmt->value, nullptr);
        if (is_void(type) || is_error(type) || is_never(type)) {
          return;
        }
        if (is_must_use(type)) {
          const u32 index = bag.emit(
              diag::Severity::Warning, kAnalyzerMustUse, expr_stmt->value->span,
              "unused Result/Option value; bind or discard it explicitly");
          (void)index;
          return;
        }
        const u32 index = bag.emit(
            diag::Severity::Warning, kAnalyzerMustUse, expr_stmt->value->span,
            "unused non-() value; discard it explicitly with `_ := ...`");
        (void)index;
        return;
      }
    }
  }

  bool contains_mut_ref(ir::TypeIdx idx, std::vector<u32>& visited) {
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
             vidx.idx <
             enum_type.variants.head().idx + enum_type.variants.size();
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

  void check_fn(u32 module, const ast::FnItem* fn, const ir::TypeIdx* self) {
    fn_ret = builder.primitive(ir::TypeTag::Void);
    if (fn->return_type != nullptr) {
      fn_ret = resolve_type(module, fn->return_type, self);
    }
    in_fn = true;
    scopes.emplace_back();
    loop_depth = 0;
    for (const ast::FnParam& param : fn->params) {
      const ir::TypeIdx type = resolve_type(module, param.type, self);
      if (bind_pattern(module, param.pattern, type)) {
        const u32 index = bag.emit(diag::Severity::Error, kAnalyzerRefutableLet,
                                   param.pattern->span,
                                   "refutable pattern in function parameter");
        (void)index;
      }
    }
    if (fn->body != nullptr) {
      check_block(module, fn->body, &fn_ret);
    }
    scopes.pop_back();
    in_fn = false;
  }

  void check_main(u32 module, const ast::FnItem* fn) {
    if (!fn->params.empty()) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadReturn,
                                 fn->span, "'main' must take no parameters");
      (void)index;
    }
    ir::TypeIdx ret = builder.primitive(ir::TypeTag::Void);
    if (fn->return_type != nullptr) {
      ret = resolve_type(module, fn->return_type, nullptr);
    }
    if (is_error(ret)) {
      return;
    }
    const ir::TypeTag tag = tag_of(ret);
    if (tag == ir::TypeTag::Void || tag == ir::TypeTag::I32) {
      return;
    }
    if (const BlessedEntry* entry = blessed_find(ret)) {
      if (entry->is_result && is_void(entry->args[0])) {
        return;
      }
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerBadReturn, fn->span,
                 "'main' must return '()', 'i32', or 'Result<(), E>'");
    (void)index;
  }

  void check_bodies() {
    for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
      for (ast::Item* item : tree.modules[m]->items) {
        switch (item->kind) {
          case ast::ItemKind::Fn: {
            const ast::FnItem* fn = static_cast<const ast::FnItem*>(item);
            check_fn(m, fn, nullptr);
            if (m == tree.root && fn->name.name == "main") {
              check_main(m, fn);
            }
            break;
          }
          case ast::ItemKind::Impl: {
            const ast::ImplItem* impl = static_cast<const ast::ImplItem*>(item);
            ir::TypeIdx self = error_type();
            const ir::TypeIdx* self_ptr = nullptr;
            if (impl->type->kind == ast::TypeKind::Path) {
              const ast::PathType* path =
                  static_cast<const ast::PathType*>(impl->type);
              if (path->args.empty()) {
                u32 target_module = kNoModule;
                std::string_view target_name;
                if (resolve_type_path(m, path->path, target_module,
                                      target_name)) {
                  if (NominalEntry* entry =
                          find_nominal(target_module, target_name)) {
                    self = intern_nominal(*entry);
                    self_ptr = &self;
                  }
                }
              }
            }
            for (const ast::FnItem* method : impl->methods) {
              check_fn(m, method, self_ptr);
            }
            break;
          }
          case ast::ItemKind::Static:
          case ast::ItemKind::Const: {
            std::string_view name;
            const ast::Type* type = nullptr;
            const ast::Expr* init = nullptr;
            const bool is_const = item->kind == ast::ItemKind::Const;
            if (is_const) {
              const ast::ConstItem* decl =
                  static_cast<const ast::ConstItem*>(item);
              name = decl->name.name;
              type = decl->type;
              init = decl->init;
            } else {
              const ast::StaticItem* decl =
                  static_cast<const ast::StaticItem*>(item);
              name = decl->name.name;
              type = decl->type;
              init = decl->init;
            }
            const ir::TypeIdx declared = resolve_type(m, type, nullptr);
            if (is_const && init->kind != ast::ExprKind::Literal) {
              const u32 index = bag.emit(
                  diag::Severity::Error, kAnalyzerInvalidOperation, init->span,
                  "const '{}' admits literal expressions only", name);
              (void)index;
            }
            if (!is_const) {
              std::vector<u32> visited;
              if (contains_mut_ref(declared, visited)) {
                const u32 index = bag.emit(
                    diag::Severity::Error, kAnalyzerInvalidOperation,
                    type->span, "static '{}' must not contain '&mut'", name);
                (void)index;
              }
            }
            in_fn = false;
            scopes.emplace_back();
            const ir::TypeIdx actual = check_expr(m, init, &declared);
            unify(declared, actual, init->span, "item initializer");
            scopes.pop_back();
            break;
          }
          default: break;
        }
      }
    }
  }

  CheckedModule empty_module(u32 module) {
    CheckedModule checked;
    checked.module = module;
    return checked;
  }
};

}  // namespace

diag::Fallible<CheckedPackage> check_package(const ModuleTree& tree,
                                             ir::PointerWidth width,
                                             diag::DiagBag& bag) {
  Checker checker{tree, width, bag};
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
  CheckedPackage package{
      tree, std::move(storage), std::move(checker.modules), {}};
  for (const BlessedEntry& entry : checker.blessed) {
    package.blessed.push_back({entry.is_result, entry.type, entry.args});
  }
  return base::make_ok(std::move(package));
}

}  // namespace analyzer
