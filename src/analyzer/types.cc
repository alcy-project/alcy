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
constexpr u32 kAnalyzerMutField = 4216;
constexpr u32 kAnalyzerUnsupportedType = 4217;

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
        ir::TypeIdx field_type =
            resolve_type(entry.module, field.type, nullptr);
        if (field.type->kind == ast::TypeKind::Ref) {
          const ast::RefType* ref =
              static_cast<const ast::RefType*>(field.type);
          if (ref->is_mut) {
            const u32 index = bag.emit(
                diag::Severity::Error, kAnalyzerMutField, field.type->span,
                "mutable references cannot be stored in struct fields");
            (void)index;
          }
        }
        fields.push_back(field_type);
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
  bool resolve_type_path(u32 module,
                         const ast::Path* path,
                         u32& module_out,
                         std::string_view& name_out) {
    if (path->segments.empty()) {
      return false;
    }
    const std::string_view head = path->segments[0].name;
    if (path->segments.size() == 1) {
      module_out = module;
      name_out = head;
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
        const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                                   path->span, "unresolved type '{}'", head);
        (void)index;
        return false;
      }
    }
    for (usize i = 1; i + 1 < path->segments.size(); ++i) {
      current = find_child_module(current, path->segments[i].name);
      if (current == kNoModule) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnknownType, path->span,
                     "unresolved type '{}'", path->segments[i].name);
        (void)index;
        return false;
      }
    }
    module_out = current;
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
  ir::Storage storage = std::move(checker.builder).build();
  checker.validate_cycles(storage);
  return base::make_ok(
      CheckedPackage{tree, std::move(storage), std::move(checker.modules)});
}

}  // namespace analyzer
