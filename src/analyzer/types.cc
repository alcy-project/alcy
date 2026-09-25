// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
#include "fpag/str/string_pool_id.h"
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

struct BlessedEntry {
  bool is_result;
  std::vector<ir::TypeIdx> args;
  ir::TypeIdx type;
};

class Checker {
 public:
  Checker(const ModuleTree& tree,
          ir::PointerWidth width,
          ast::AstArena& ast,
          diag::DiagBag& bag)
      : tree(tree),
        ast(ast),
        width(width),
        bag(bag),
        interner(kInternerCapacity) {}

  const ModuleTree& tree;
  ast::AstArena& ast;
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

  // Pass 1: registers every nominal definition, diagnosing duplicates
  // and reserved names. No interning happens here.
  void register_nominals() {
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
        seq.push(builder.ref_type(field));
      }
      builder.fill_struct(entry.type, seq.finish());
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

  bool resolve_type_path(u32 module,
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

  ir::TypeIdx resolve_type(u32 module,
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
        for (ast::TypeIdx element :
             node.payload.get<ast::TypeTuple>().elements) {
          elements.push_back(resolve_type(module, element, self));
        }
        ir::TypeSeq seq;
        for (ir::TypeIdx element : elements) {
          seq.push(builder.ref_type(element));
        }
        return builder.tuple_type(seq.finish());
      }
      case ast::TypeKind::Ref: {
        ir::TypeIdx pointee =
            resolve_type(module, node.payload.get<ast::TypeRef>().inner, self);
        return builder.reference_type(pointee,
                                      node.payload.get<ast::TypeRef>().is_mut);
      }
      case ast::TypeKind::Path: {
        const ast::Path& path =
            ast.paths[node.payload.get<ast::TypePath>().path];
        if (path.segments.size() == 1) {
          const std::string_view name = path.segments[0].name;
          if (name == "Self") {
            if (self == nullptr) {
              const u32 index =
                  bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                           node.span, "Self outside of an impl block");
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
          if (name == "Result" || name == "Option") {
            const bool is_result = name == "Result";
            const usize want = is_result ? 2 : 1;
            if (node.payload.get<ast::TypePath>().args.size() != want) {
              const u32 index =
                  bag.emit(diag::Severity::Error, kAnalyzerArityMismatch,
                           node.span, "'{}' expects {} argument{}", name, want,
                           want == 1 ? "" : "s");
              (void)index;
              return error_type();
            }
            std::vector<ir::TypeIdx> args;
            args.reserve(node.payload.get<ast::TypePath>().args.size());
            for (ast::TypeIdx arg : node.payload.get<ast::TypePath>().args) {
              args.push_back(resolve_type(module, arg, self));
            }
            return intern_blessed(is_result, args);
          }
        }
        if (!node.payload.get<ast::TypePath>().args.empty()) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerGenericArguments,
                       node.span, "generic arguments are not supported");
          (void)index;
          return error_type();
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
        return intern_nominal(*entry);
      }
    }
  }

  // Closed compiler-known intrinsic set (see docs/spec/items.md).
  // `print`/`println`/`panic` stay callable without a declaration
  // until core provides them; `memcopy` requires one.
  static bool is_known_intrinsic(std::string_view name) {
    return name == "memcopy" || name == "print" || name == "println" ||
           name == "panic" || name == "str_len" || name == "str_byte" ||
           name == "str_slice";
  }

  // Verifies a declared intrinsic signature against its canonical
  // shape; declarations are documentation-checked, never trusted.
  bool check_intrinsic_signature(u32 module,
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
    } else {
      return false;
    }
    if (params.size() != expected.size() || ret.idx != expected_ret.idx) {
      const u32 index = bag.emit(
          diag::Severity::Error, kAnalyzerInvalidOperation, intrinsic.name.span,
          "intrinsic '{}' has the wrong signature", name);
      (void)index;
      return false;
    }
    for (usize i = 0; i < params.size(); ++i) {
      if (params[i].idx != expected[i].idx) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                     intrinsic.name.span,
                     "intrinsic '{}' has the wrong signature", name);
        (void)index;
        return false;
      }
    }
    (void)module;
    return true;
  }

  void process_module(u32 module) {
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
            entry = find_nominal(module,
                                 node.payload.get<ast::ItemEnum>().name.name);
          }
          if (entry != nullptr) {
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
              {node.payload.get<ast::ItemFn>().name.name, std::move(params),
               ret, item});
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
          const ast::TypeNode& self_node =
              ast.types[node.payload.get<ast::ItemImpl>().type];
          if (self_node.kind == ast::TypeKind::Path) {
            if (!self_node.payload.get<ast::TypePath>().args.empty()) {
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
                  self_type = intern_nominal(*entry);
                  self_ok = true;
                } else {
                  const u32 index = bag.emit(
                      diag::Severity::Error, kAnalyzerUnknownType,
                      self_node.span, "inherent impl requires a nominal type");
                  (void)index;
                }
              }
            }
          } else {
            const u32 index = bag.emit(diag::Severity::Error,
                                       kAnalyzerUnknownType, self_node.span,
                                       "inherent impl requires a nominal type");
            (void)index;
          }
          for (ast::ItemIdx method :
               node.payload.get<ast::ItemImpl>().methods) {
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

  // ---- Expression checking ----

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

  // User-facing type names: the table spells bool as `i1` and unit as
  // `void`, which read poorly in diagnostics.
  static const char* pretty_tag(ir::TypeTag tag) {
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
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, span,
                 "type mismatch in {}: expected '{}', found '{}'", what,
                 pretty_tag(tag_of(expected)), pretty_tag(tag_of(actual)));
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

  ir::TypeIdx check_literal(ast::LiteralIdx value,
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
      if (entry.module != module) {
        continue;
      }
      const ast::ItemNode& node = ast.items[entry.item];
      if (node.kind != ast::ItemKind::Enum) {
        continue;
      }
      for (u32 i = 0;
           i <
           static_cast<u32>(node.payload.get<ast::ItemEnum>().variants.size());
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

  void record_call(u32 module,
                   ast::ExprIdx callee,
                   const CheckedModule::FnSig* fn) {
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      for (u32 i = 0; i < static_cast<u32>(modules[m].functions.size()); ++i) {
        if (&modules[m].functions[i] == fn) {
          modules[module].call_targets.push_back({callee, false, m, i});
          return;
        }
      }
    }
  }

  void record_call(u32 module,
                   ast::ExprIdx callee,
                   const CheckedModule::MethodInfo* method) {
    for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
      for (u32 i = 0; i < static_cast<u32>(modules[m].methods.size()); ++i) {
        if (&modules[m].methods[i] == method) {
          modules[module].call_targets.push_back({callee, true, m, i});
          return;
        }
      }
    }
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
  bool resolve_value_path(u32 module, ast::PathIdx path, PathValue& out) {
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
      VariantMatch match;
      if (find_variant(module, name, node.span, match)) {
        const ast::ItemNode& decl = ast.items[match.enom->item];
        out.enom = match.enom;
        out.variant = match.variant;
        out.type = intern_nominal(*match.enom);
        if (decl.payload.get<ast::ItemEnum>()
                .variants[match.variant]
                .fields.empty()) {
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
          out.type = intern_nominal(*matches[0].enom);
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
               i <
               static_cast<u32>(
                   nominal_node.payload.get<ast::ItemEnum>().variants.size());
               ++i) {
            if (nominal_node.payload.get<ast::ItemEnum>()
                    .variants[i]
                    .name.name == member) {
              out.enom = nominal;
              out.variant = i;
              out.type = intern_nominal(*nominal);
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
  // in scope, `Enum::Variant`, `module::Variant`, or a blessed
  // constructor (`Ok`, `Err`, `Some`, `None`; the instantiation is
  // fixed later against the scrutinee type, so blessed stays null).
  bool resolve_variant_path(u32 module, ast::PathIdx path, PathValue& out) {
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
          out.type = intern_nominal(*matches[0].enom);
          return true;
        }
        return false;
      }
      if (NominalEntry* nominal = find_nominal_in_scope(module, head)) {
        const ast::ItemNode& nominal_node = ast.items[nominal->item];
        if (nominal_node.kind == ast::ItemKind::Enum) {
          for (u32 i = 0;
               i <
               static_cast<u32>(
                   nominal_node.payload.get<ast::ItemEnum>().variants.size());
               ++i) {
            if (nominal_node.payload.get<ast::ItemEnum>()
                    .variants[i]
                    .name.name == member) {
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

  NominalEntry* resolve_struct_path(u32 module, ast::PathIdx path) {
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

  // ---- Patterns ----

  void collect_pattern_idents(ast::PatternIdx pattern,
                              std::vector<std::string_view>& out) {
    switch (ast.patterns[pattern].kind) {
      case ast::PatternKind::Wildcard:
      case ast::PatternKind::Literal: return;
      case ast::PatternKind::Ident: {
        out.push_back(ast.patterns[pattern].payload.ident.name.name);
        return;
      }
      case ast::PatternKind::MutIdent: {
        out.push_back(ast.patterns[pattern].payload.mut_ident.name.name);
        return;
      }
      case ast::PatternKind::Tuple: {
        for (ast::PatternIdx element :
             ast.patterns[pattern].payload.tuple.elements) {
          collect_pattern_idents(element, out);
        }
        return;
      }
      case ast::PatternKind::Struct: {
        for (const ast::FieldPattern& field :
             ast.patterns[pattern].payload.strukt.fields) {
          collect_pattern_idents(field.pattern, out);
        }
        return;
      }
      case ast::PatternKind::Ref: {
        collect_pattern_idents(ast.patterns[pattern].payload.ref.inner, out);
        return;
      }
      case ast::PatternKind::Or: {
        for (ast::PatternIdx alt :
             ast.patterns[pattern].payload.or_pat.alternatives) {
          collect_pattern_idents(alt, out);
        }
        return;
      }
    }
  }

  void bind_error_idents(u32 module, ast::PatternIdx pattern) {
    std::vector<std::string_view> names;
    collect_pattern_idents(pattern, names);
    (void)module;
    for (std::string_view name : names) {
      scopes.back().push_back({name, error_type(), false});
    }
  }

  // Binds a pattern against a type, declaring locals. Returns true
  // when the pattern is refutable (declarations reject those).
  // `bind_comp_known` marks the declared locals comp-known for `comp`
  // parameters and declarations; anything bound while checking comp
  // evaluation contexts counts as comp-known as well.
  bool bind_comp_known = false;
  bool bind_pattern(u32 module, ast::PatternIdx pattern, ir::TypeIdx type) {
    const bool comp = bind_comp_known || comp_depth > 0;
    const ast::PatternNode& node = ast.patterns[pattern];
    switch (node.kind) {
      case ast::PatternKind::Wildcard: return false;
      case ast::PatternKind::Ident: {
        scopes.back().push_back(
            {node.payload.ident.name.name, type, false, comp});
        return false;
      }
      case ast::PatternKind::MutIdent: {
        scopes.back().push_back(
            {node.payload.mut_ident.name.name, type, true, comp});
        return false;
      }
      case ast::PatternKind::Literal: {
        check_literal(node.payload.literal.value, &type);
        return true;
      }
      case ast::PatternKind::Tuple: {
        if (node.payload.tuple.path.is_valid()) {
          PathValue resolved;
          if (!resolve_variant_path(module, node.payload.tuple.path,
                                    resolved)) {
            bind_error_idents(module, pattern);
            return true;
          }
          if (resolved.kind != PathValue::Kind::TupleVariant &&
              resolved.kind != PathValue::Kind::BlessedCtor) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                         node.span, "not a tuple variant");
            (void)index;
            bind_error_idents(module, pattern);
            return true;
          }
          if (resolved.kind == PathValue::Kind::TupleVariant) {
            unify(type, resolved.type, node.span, "tuple variant pattern");
          }
          std::vector<ir::TypeIdx> payloads =
              variant_payloads(resolved, type, node.span);
          const std::span<const ast::PatternIdx> elements =
              node.payload.tuple.elements;
          if (payloads.size() != elements.size()) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerArityError, node.span,
                         "variant expects {} fields, pattern has {}",
                         payloads.size(), elements.size());
            (void)index;
            bind_error_idents(module, pattern);
            return true;
          }
          bool refutable = true;
          for (usize i = 0; i < payloads.size(); ++i) {
            refutable =
                bind_pattern(module, elements[i], payloads[i]) && refutable;
          }
          return refutable;
        }
        if (tag_of(type) != ir::TypeTag::Tuple) {
          unify(type, builder.tuple_type({ir::TypeIdx(0), 0}), node.span,
                "tuple pattern");
          bind_error_idents(module, pattern);
          return false;
        }
        const ir::TupleType& tuple_type =
            builder.tuple_types()[builder.types()[type].as_tuple()];
        const std::span<const ast::PatternIdx> elements =
            node.payload.tuple.elements;
        if (tuple_type.elements.size() != elements.size()) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, node.span,
                       "tuple pattern has {} elements, type has {}",
                       elements.size(), tuple_type.elements.size());
          (void)index;
          bind_error_idents(module, pattern);
          return false;
        }
        bool refutable = false;
        for (u32 i = 0; i < static_cast<u32>(elements.size()); ++i) {
          if (bind_pattern(module, elements[i], tuple_type.elements[i])) {
            refutable = true;
          }
        }
        return refutable;
      }
      case ast::PatternKind::Struct: {
        NominalEntry* nominal =
            resolve_struct_path(module, node.payload.strukt.path);
        if (nominal == nullptr) {
          bind_error_idents(module, pattern);
          return false;
        }
        unify(type, intern_nominal(*nominal), node.span, "struct pattern");
        const ast::ItemNode& decl = ast.items[nominal->item];
        const ir::StructType& struct_type =
            builder.struct_types()[builder.types()[nominal->type].as_struct()];
        bool refutable = false;
        for (const ast::FieldPattern& field : node.payload.strukt.fields) {
          u32 index = 0;
          bool found = false;
          for (const ast::ItemStructField& decl_field :
               decl.payload.get<ast::ItemStruct>().fields) {
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
        const ir::TypeTag tag = tag_of(type);
        if ((node.payload.ref.is_mut && tag != ir::TypeTag::MutRef) ||
            (!node.payload.ref.is_mut && tag != ir::TypeTag::Ref)) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, node.span,
                       "reference pattern on a non-reference type");
          (void)index;
          bind_error_idents(module, pattern);
          return false;
        }
        const ir::TypeIdx pointee =
            builder.ref_types()[builder.types()[type].as_ref()].pointee;
        return bind_pattern(module, node.payload.ref.inner, pointee);
      }
      case ast::PatternKind::Or: {
        const std::span<const ast::PatternIdx> alternatives =
            node.payload.or_pat.alternatives;
        if (alternatives.empty()) {
          return false;
        }
        const usize alt0_start = scopes.back().size();
        bool refutable = bind_pattern(module, alternatives[0], type);
        const usize base = scopes.back().size();
        for (usize i = 1; i < alternatives.size(); ++i) {
          const usize mark = scopes.back().size();
          if (bind_pattern(module, alternatives[i], type)) {
            refutable = true;
          }
          // Every alternative must bind the same names; extras are
          // dropped with a diagnostic.
          for (usize j = mark; j < scopes.back().size(); ++j) {
            bool found = false;
            for (usize k = alt0_start; k < base; ++k) {
              if (scopes.back()[k].name == scopes.back()[j].name) {
                found = true;
                break;
              }
            }
            if (!found) {
              const u32 index =
                  bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                           ast.patterns[alternatives[i]].span,
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

  bool is_bare_int_literal(ast::ExprIdx expr) const {
    const ast::ExprNode& node = ast.exprs[expr];
    if (node.kind != ast::ExprKind::Literal) {
      return false;
    }
    const ast::Literal& value =
        ast.literals[node.payload.get<ast::ExprLiteral>().value];
    if (value.kind != ast::LiteralKind::Integer) {
      return false;
    }
    const std::string_view spelling = value.spelling;
    for (char c : spelling) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return false;
      }
    }
    return true;
  }

  // Structural comp-known-ness over checked expressions: literals,
  // comp-known locals and literal consts, and pure combinations
  // thereof. Calls count when their arguments are comp-known and the
  // callee is not an intrinsic (print/panic never are).
  bool expr_comp_known(u32 module, ast::ExprIdx expr) const {
    const ast::ExprNode& node = ast.exprs[expr];
    switch (node.kind) {
      case ast::ExprKind::Literal: {
        const ast::Literal& value =
            ast.literals[node.payload.get<ast::ExprLiteral>().value];
        return value.kind == ast::LiteralKind::Integer ||
               value.kind == ast::LiteralKind::Bool ||
               value.kind == ast::LiteralKind::String;
      }
      case ast::ExprKind::Path: {
        const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
        const std::span<const ast::Ident> segments = ast.paths[path].segments;
        if (segments.size() != 1) {
          return false;
        }
        if (const Local* local = lookup_local(segments[0].name)) {
          return local->comp_known;
        }
        return is_literal_const(module, path);
      }
      case ast::ExprKind::Unary:
        return expr_comp_known(module,
                               node.payload.get<ast::ExprUnary>().inner);
      case ast::ExprKind::Borrow:
        return expr_comp_known(module,
                               node.payload.get<ast::ExprBorrow>().inner);
      case ast::ExprKind::Binary:
        return expr_comp_known(module,
                               node.payload.get<ast::ExprBinary>().lhs) &&
               expr_comp_known(module, node.payload.get<ast::ExprBinary>().rhs);
      case ast::ExprKind::Cast:
        return expr_comp_known(module, node.payload.get<ast::ExprCast>().inner);
      case ast::ExprKind::Tuple: {
        for (ast::ExprIdx element :
             node.payload.get<ast::ExprTuple>().elements) {
          if (!expr_comp_known(module, element)) {
            return false;
          }
        }
        return true;
      }
      case ast::ExprKind::Struct: {
        for (const ast::ExprFieldInit& field :
             node.payload.get<ast::ExprStruct>().init) {
          if (!expr_comp_known(module, field.value)) {
            return false;
          }
        }
        return !node.payload.get<ast::ExprStruct>().base_expr.is_valid() ||
               expr_comp_known(module,
                               node.payload.get<ast::ExprStruct>().base_expr);
      }
      case ast::ExprKind::Field:
        return expr_comp_known(module,
                               node.payload.get<ast::ExprField>().receiver);
      case ast::ExprKind::Index:
        // Fixed arrays are outside the comp domain for now.
        return false;
      case ast::ExprKind::Call: {
        for (ast::ExprIdx arg : node.payload.get<ast::ExprCall>().args) {
          if (!expr_comp_known(module, arg)) {
            return false;
          }
        }
        const ast::ExprIdx callee = node.payload.get<ast::ExprCall>().callee;
        if (ast.exprs[callee].kind != ast::ExprKind::Path) {
          return false;
        }
        const ast::PathIdx path =
            ast.exprs[callee].payload.get<ast::ExprPath>().idx;
        const std::span<const ast::Ident> segments = ast.paths[path].segments;
        if (segments.size() == 1) {
          const std::string_view name = segments[0].name;
          if (name == "print" || name == "println" || name == "panic") {
            return false;
          }
        }
        return true;
      }
      case ast::ExprKind::MethodCall: {
        if (!expr_comp_known(
                module, node.payload.get<ast::ExprMethodCall>().receiver)) {
          return false;
        }
        for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
          if (!expr_comp_known(module, arg)) {
            return false;
          }
        }
        return true;
      }
      case ast::ExprKind::Question:
        return expr_comp_known(module,
                               node.payload.get<ast::ExprQuestion>().inner);
      case ast::ExprKind::If: {
        const ast::Cond& cond = ast.conds[node.payload.get<ast::ExprIf>().cond];
        if (cond.is_pattern || !expr_comp_known(module, cond.value)) {
          return false;
        }
        if (!expr_comp_known_block(
                module, node.payload.get<ast::ExprIf>().then_block) ||
            (node.payload.get<ast::ExprIf>().else_block.is_valid() &&
             !expr_comp_known_block(
                 module, node.payload.get<ast::ExprIf>().else_block))) {
          return false;
        }
        return true;
      }
      case ast::ExprKind::Match: {
        if (!expr_comp_known(module,
                             node.payload.get<ast::ExprMatch>().scrutinee)) {
          return false;
        }
        for (const ast::ExprMatchArm& arm :
             node.payload.get<ast::ExprMatch>().arms) {
          if (!expr_comp_known(module, arm.body)) {
            return false;
          }
        }
        return true;
      }
      case ast::ExprKind::Block:
        return expr_comp_known_block(module,
                                     node.payload.get<ast::ExprBlock>().block);
      case ast::ExprKind::Loop:
        return expr_comp_known_block(module,
                                     node.payload.get<ast::ExprLoop>().body);
      case ast::ExprKind::While: {
        const ast::Cond& cond =
            ast.conds[node.payload.get<ast::ExprWhile>().cond];
        if (cond.is_pattern || !expr_comp_known(module, cond.value)) {
          return false;
        }
        return expr_comp_known_block(module,
                                     node.payload.get<ast::ExprWhile>().body);
      }
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue: return true;
      case ast::ExprKind::Return:
      case ast::ExprKind::Range: return false;
    }
  }

  bool expr_comp_known_block(u32 module, ast::BlockIdx block) const {
    const ast::Block& node = ast.blocks[block];
    for (ast::StmtIdx stmt : node.statements) {
      const ast::StmtNode& stmt_node = ast.stmts[stmt];
      if (stmt_node.kind == ast::StmtKind::Decl) {
        if (!expr_comp_known(module,
                             stmt_node.payload.get<ast::StmtDecl>().init)) {
          return false;
        }
        continue;
      }
      if (stmt_node.kind == ast::StmtKind::Reassign) {
        const ast::StmtReassign& reassign =
            stmt_node.payload.get<ast::StmtReassign>();
        if (ast.exprs[reassign.place].kind != ast::ExprKind::Path ||
            !expr_comp_known(module, reassign.value)) {
          return false;
        }
        continue;
      }
      if (stmt_node.kind == ast::StmtKind::Expr) {
        const ast::ExprKind kind =
            ast.exprs[stmt_node.payload.get<ast::StmtExpr>().value].kind;
        if (kind == ast::ExprKind::Break || kind == ast::ExprKind::Continue) {
          continue;
        }
      }
      return false;
    }
    return !node.value.is_valid() || expr_comp_known(module, node.value);
  }

  // Checks operands of a binary operator: same-type numerics, with
  // bare integer literals coerced to the other side (so `x + 42`
  // works for any integer x without an annotation).
  ir::TypeIdx check_binary_operands(u32 module,
                                    ast::ExprIdx lhs,
                                    ast::ExprIdx rhs,
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
                 pretty_tag(tag_of(left)), pretty_tag(tag_of(right)));
    (void)index;
    return error_type();
  }

  void check_call_args(u32 module,
                       std::span<const ast::ExprIdx> args,
                       const std::vector<ir::TypeIdx>& params,
                       const std::vector<bool>& comp_params,
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
      unify(param, actual, ast.exprs[args[i]].span, "argument");
      const bool comp_required =
          comp_depth > 0 ||
          (fixed + i < comp_params.size() && comp_params[fixed + i]);
      if (comp_required && !comp_checked_in_scope(args[i]) &&
          !expr_comp_known(module, args[i])) {
        if (comp_depth > 0) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                       ast.exprs[args[i]].span,
                       "comp evaluation argument must be comp-known");
          (void)index;
        } else {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                       ast.exprs[args[i]].span,
                       "argument for `comp` parameter must be comp-known");
          (void)index;
        }
      }
    }
  }

  // Comp flags of a resolved function item, in parameter order.
  std::vector<bool> comp_param_flags(ast::ItemIdx item) const {
    std::vector<bool> flags;
    if (!item.is_valid()) {
      return flags;
    }
    const ast::ItemNode& node = ast.items[item];
    if (node.kind != ast::ItemKind::Fn) {
      return flags;
    }
    for (const ast::ItemFnParam& param :
         node.payload.get<ast::ItemFn>().params) {
      flags.push_back(param.is_comp);
    }
    return flags;
  }

  // Comp blocks verify their contents in-scope while checking;
  // re-checking them afterwards would see popped scopes. Only
  // non-block initializers need the post-check here.
  bool comp_checked_in_scope(ast::ExprIdx init) const {
    return ast.exprs[init].kind == ast::ExprKind::Block &&
           ast.exprs[init].payload.get<ast::ExprBlock>().is_comp;
  }

  // Literal consts (inline constants) are readable in comp
  // evaluation; anything else with storage is not.
  bool is_literal_const(u32 module, ast::PathIdx path) const {
    const std::span<const ast::Ident> segments = ast.paths[path].segments;
    if (segments.size() != 1) {
      return false;
    }
    const CheckedModule::StaticInfo* info =
        lookup_static(module, segments[0].name);
    return info != nullptr && info->is_const && info->init.is_valid() &&
           ast.exprs[info->init].kind == ast::ExprKind::Literal;
  }

  ir::TypeIdx check_path_expr(u32 module,
                              ast::PathIdx path,
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
        if (resolved.kind == PathValue::Kind::Static && comp_depth > 0 &&
            !is_literal_const(module, path)) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerInvalidComp, span,
              "statics with storage cannot be read in comp evaluation");
          (void)index;
          return error_type();
        }
        if (resolved.kind == PathValue::Kind::UnitVariant) {
          modules[module].variants.push_back(
              {path, false, true, resolved.type, resolved.variant});
        }
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
              modules[module].variants.push_back(
                  {path, true, false, *expected, 0});
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
                         ast::ExprIdx expr,
                         const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprIdx callee = node.payload.get<ast::ExprCall>().callee;
    const std::span<const ast::ExprIdx> args =
        node.payload.get<ast::ExprCall>().args;
    const diag::Span span = node.span;
    if (ast.exprs[callee].kind != ast::ExprKind::Path) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                   ast.exprs[callee].span, "cannot call a non-path expression");
      (void)index;
      for (ast::ExprIdx arg : args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    const ast::PathIdx path =
        ast.exprs[callee].payload.get<ast::ExprPath>().idx;
    const std::span<const ast::Ident> segments = ast.paths[path].segments;
    if (segments.size() == 1 && lookup_local(segments[0].name) == nullptr &&
        lookup_static(module, segments[0].name) == nullptr &&
        lookup_function(module, segments[0].name) == nullptr) {
      const std::string_view name = segments[0].name;
      if (name == "print" || name == "println") {
        if (comp_depth > 0) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, span,
                       "'{}' is not allowed in comp evaluation", name);
          (void)index;
          return error_type();
        }
        if (args.size() != 1) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                       "'{}' expects 1 argument, found {}", name, args.size());
          (void)index;
          return error_type();
        }
        const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
        const ir::TypeIdx actual = check_expr(module, args[0], &str);
        unify(str, actual, ast.exprs[args[0]].span, "print argument");
        const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
        if (expected != nullptr) {
          return unify(*expected, unit, span, "call");
        }
        return unit;
      }
      if (name == "panic") {
        if (comp_depth > 0) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, span,
                       "'panic' is not allowed in comp evaluation");
          (void)index;
          return error_type();
        }
        if (args.size() != 1) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                       "'panic' expects 1 argument, found {}", args.size());
          (void)index;
          return error_type();
        }
        const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
        const ir::TypeIdx actual = check_expr(module, args[0], &str);
        unify(str, actual, ast.exprs[args[0]].span, "panic argument");
        return builder.never_type();
      }
    }
    PathValue resolved;
    if (!resolve_value_path(module, path, resolved)) {
      for (ast::ExprIdx arg : args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    if (resolved.kind == PathValue::Kind::Function) {
      const CheckedModule::FnSig* fn = resolved.function;
      record_call(module, callee, fn);
      check_call_args(module, args, fn->params, comp_param_flags(fn->item),
                      span, fn->name, false);
      if (expected != nullptr) {
        return unify(*expected, fn->ret, span, "call");
      }
      return fn->ret;
    }
    if (resolved.kind == PathValue::Kind::AssocFunction) {
      const CheckedModule::MethodInfo* method = resolved.method;
      record_call(module, callee, method);
      // Associated functions take no receiver; params map 1:1.
      check_call_args(module, args, method->params,
                      comp_param_flags(method->item), span, method->name,
                      false);
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
          for (ast::ExprIdx arg : args) {
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
      if (resolved.kind == PathValue::Kind::TupleVariant) {
        modules[module].variants.push_back(
            {path, false, true, enum_type, resolved.variant});
      } else {
        modules[module].variants.push_back(
            {path, true, resolved.blessed_first, enum_type, 0});
      }
      if (args.size() != payloads.size()) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                     "variant expects {} arguments, found {}", payloads.size(),
                     args.size());
        (void)index;
        for (ast::ExprIdx arg : args) {
          check_expr(module, arg, nullptr);
        }
        return error_type();
      }
      for (usize i = 0; i < args.size(); ++i) {
        const ir::TypeIdx actual = check_expr(module, args[i], &payloads[i]);
        unify(payloads[i], actual, ast.exprs[args[i]].span, "variant argument");
        if (comp_depth > 0 && !comp_checked_in_scope(args[i]) &&
            !expr_comp_known(module, args[i])) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                       ast.exprs[args[i]].span,
                       "comp evaluation argument must be comp-known");
          (void)index;
        }
      }
      if (expected != nullptr) {
        return unify(*expected, enum_type, span, "call");
      }
      return enum_type;
    }
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                               span, "not callable");
    (void)index;
    for (ast::ExprIdx arg : args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }

  ir::TypeIdx check_method_call(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ir::TypeIdx receiver = check_expr(
        module, node.payload.get<ast::ExprMethodCall>().receiver, nullptr);
    if (is_error(receiver)) {
      for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    const std::string_view name =
        node.payload.get<ast::ExprMethodCall>().name.name;
    const diag::Span span = node.span;
    if (const BlessedEntry* entry = blessed_find(receiver)) {
      const ir::TypeIdx ok = entry->args[0];
      if (name == "unwrap") {
        if (!node.payload.get<ast::ExprMethodCall>().args.empty()) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                       "'unwrap' expects 0 arguments, found {}",
                       node.payload.get<ast::ExprMethodCall>().args.size());
          (void)index;
          return error_type();
        }
        if (expected != nullptr) {
          return unify(*expected, ok, span, "method call");
        }
        return ok;
      }
      if (name == "expect") {
        if (node.payload.get<ast::ExprMethodCall>().args.size() != 1) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                       "'expect' expects 1 argument, found {}",
                       node.payload.get<ast::ExprMethodCall>().args.size());
          (void)index;
          return error_type();
        }
        const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
        const ir::TypeIdx actual = check_expr(
            module, node.payload.get<ast::ExprMethodCall>().args[0], &str);
        unify(str, actual,
              ast.exprs[node.payload.get<ast::ExprMethodCall>().args[0]].span,
              "expect argument");
        if (expected != nullptr) {
          return unify(*expected, ok, span, "method call");
        }
        return ok;
      }
      if (name == "is_ok" || name == "is_err") {
        if (!node.payload.get<ast::ExprMethodCall>().args.empty()) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                       "'{}' expects 0 arguments, found {}", name,
                       node.payload.get<ast::ExprMethodCall>().args.size());
          (void)index;
          return error_type();
        }
        const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
        if (expected != nullptr) {
          return unify(*expected, boolean, span, "method call");
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
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                   node.payload.get<ast::ExprMethodCall>().name.span,
                   "no method '{}'", name);
      (void)index;
      for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    if (method->receiver == CheckedModule::ReceiverKind::None) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                   node.payload.get<ast::ExprMethodCall>().name.span,
                   "associated function '{}' called as a method", name);
      (void)index;
      return error_type();
    }
    record_call(module, expr, method);
    // No autoref/deref in MVP beyond this: an owned receiver coerces
    // to the declared borrow; full borrow checking is a later stage.
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
        const u32 index = bag.emit(
            diag::Severity::Error, kAnalyzerTypeMismatch,
            ast.exprs[node.payload.get<ast::ExprMethodCall>().receiver].span,
            "type mismatch in receiver");
        (void)index;
        return error_type();
      }
    }
    std::vector<ir::TypeIdx> rest(method->params.begin() + 1,
                                  method->params.end());
    std::vector<bool> comp_flags = comp_param_flags(method->item);
    // The receiver has no call-site argument; drop its flag with it.
    std::vector<bool> rest_flags;
    if (comp_flags.size() == method->params.size() && !comp_flags.empty()) {
      rest_flags.assign(comp_flags.begin() + 1, comp_flags.end());
    }
    check_call_args(module, node.payload.get<ast::ExprMethodCall>().args, rest,
                    rest_flags, span, name, false);
    if (expected != nullptr) {
      return unify(*expected, method->ret, span, "method call");
    }
    return method->ret;
  }

  ir::TypeIdx check_field(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::Ident field_name = node.payload.get<ast::ExprField>().name;
    ir::TypeIdx receiver = check_expr(
        module, node.payload.get<ast::ExprField>().receiver, nullptr);
    if (is_error(receiver)) {
      return error_type();
    }
    // Field access sees through references (method receivers are
    // commonly `&Self`); borrow checking is a later stage.
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
        if (!entry.complete || entry.type.idx != receiver.idx) {
          continue;
        }
        const ast::ItemNode& owner_node = ast.items[entry.item];
        if (owner_node.kind != ast::ItemKind::Struct) {
          continue;
        }
        u32 i = 0;
        for (const ast::ItemStructField& decl_field :
             owner_node.payload.get<ast::ItemStruct>().fields) {
          if (decl_field.name.name == field_name.name) {
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
                     field_name.span, "unknown field '{}'", field_name.name);
        (void)index;
        return error_type();
      }
      const ir::TypeIdx result = struct_type.fields[field_index];
      (void)module;
      if (expected != nullptr) {
        return unify(*expected, result, node.span, "field");
      }
      return result;
    }
    if (tag == ir::TypeTag::Tuple) {
      const ir::TupleType& tuple_type =
          builder.tuple_types()[builder.types()[receiver].as_tuple()];
      u32 index = 0;
      bool digits = !field_name.name.empty();
      for (char c : field_name.name) {
        if (c < '0' || c > '9') {
          digits = false;
          break;
        }
        index = index * 10 + static_cast<u32>(c - '0');
      }
      if (!digits || index >= tuple_type.elements.size()) {
        const u32 diag = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                                  field_name.span, "unknown tuple field '{}'",
                                  field_name.name);
        (void)diag;
        return error_type();
      }
      const ir::TypeIdx result = tuple_type.elements[index];
      if (expected != nullptr) {
        return unify(*expected, result, node.span, "field");
      }
      return result;
    }
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                               node.span, "no fields on '{}'", pretty_tag(tag));
    (void)index;
    return error_type();
  }

  ir::TypeIdx check_struct_expr(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    NominalEntry* nominal =
        resolve_struct_path(module, node.payload.get<ast::ExprStruct>().path);
    if (nominal == nullptr) {
      for (const ast::ExprFieldInit& field :
           node.payload.get<ast::ExprStruct>().init) {
        check_expr(module, field.value, nullptr);
      }
      if (node.payload.get<ast::ExprStruct>().base_expr.is_valid()) {
        check_expr(module, node.payload.get<ast::ExprStruct>().base_expr,
                   nullptr);
      }
      return error_type();
    }
    const ir::TypeIdx struct_type = intern_nominal(*nominal);
    const ast::ItemNode& decl = ast.items[nominal->item];
    const ir::StructType& fields =
        builder.struct_types()[builder.types()[struct_type].as_struct()];
    std::vector<bool> seen(decl.payload.get<ast::ItemStruct>().fields.size(),
                           false);
    for (const ast::ExprFieldInit& field :
         node.payload.get<ast::ExprStruct>().init) {
      u32 index = 0;
      bool found = false;
      for (const ast::ItemStructField& decl_field :
           decl.payload.get<ast::ItemStruct>().fields) {
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
      unify(field_type, actual, ast.exprs[field.value].span, "field");
    }
    if (node.payload.get<ast::ExprStruct>().base_expr.is_valid()) {
      const ir::TypeIdx base = check_expr(
          module, node.payload.get<ast::ExprStruct>().base_expr, nullptr);
      unify(struct_type, base,
            ast.exprs[node.payload.get<ast::ExprStruct>().base_expr].span,
            "struct update base");
    } else {
      for (usize i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
          const u32 diag =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, node.span,
                       "missing field '{}'",
                       decl.payload.get<ast::ItemStruct>().fields[i].name.name);
          (void)diag;
        }
      }
    }
    if (expected != nullptr) {
      return unify(*expected, struct_type, node.span, "struct");
    }
    return struct_type;
  }

  ir::TypeIdx check_question(u32 module,
                             ast::ExprIdx expr,
                             const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ir::TypeIdx inner = check_expr(
        module, node.payload.get<ast::ExprQuestion>().inner, nullptr);
    const BlessedEntry* scrutinee = blessed_find(inner);
    if (scrutinee == nullptr) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadQuestion,
                                 node.span, "'?' needs Result or Option");
      (void)index;
      return error_type();
    }
    const BlessedEntry* enclosing = blessed_find(fn_ret);
    if (enclosing == nullptr) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerBadQuestion, node.span,
                   "'?' needs an enclosing Result or Option function");
      (void)index;
      return error_type();
    }
    if (scrutinee->is_result != enclosing->is_result ||
        scrutinee->args.size() != enclosing->args.size()) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerBadQuestion, node.span,
                   "'?' type does not match the function return type");
      (void)index;
      return error_type();
    }
    for (usize i = 0; i < scrutinee->args.size(); ++i) {
      if (!types_equal(scrutinee->args[i], enclosing->args[i])) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerBadQuestion, node.span,
                     "'?' type does not match the function return type");
        (void)index;
        return error_type();
      }
    }
    if (expected != nullptr) {
      return unify(*expected, scrutinee->args[0], node.span, "'?'");
    }
    return scrutinee->args[0];
  }

  ir::TypeIdx check_cast(u32 module,
                         ast::ExprIdx expr,
                         const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ir::TypeIdx inner =
        check_expr(module, node.payload.get<ast::ExprCast>().inner, nullptr);
    const ir::TypeIdx target =
        resolve_type(module, node.payload.get<ast::ExprCast>().type, nullptr);
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
        return unify(*expected, target, node.span, "cast");
      }
      return target;
    }
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                               node.span, "invalid cast from '{}' to '{}'",
                               pretty_tag(from), pretty_tag(to));
    (void)index;
    return error_type();
  }

  ir::TypeIdx check_index(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ir::TypeIdx receiver = check_expr(
        module, node.payload.get<ast::ExprIndex>().receiver, nullptr);
    const ir::TypeIdx position =
        check_expr(module, node.payload.get<ast::ExprIndex>().index, nullptr);
    if (is_error(receiver) || is_error(position)) {
      return error_type();
    }
    if (!is_integer_tag(tag_of(position))) {
      const u32 diag =
          bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                   ast.exprs[node.payload.get<ast::ExprIndex>().index].span,
                   "array index must be an integer");
      (void)diag;
      return error_type();
    }
    if (tag_of(receiver) != ir::TypeTag::Array) {
      const u32 diag =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, node.span,
                   "cannot index '{}'", pretty_tag(tag_of(receiver)));
      (void)diag;
      return error_type();
    }
    const ir::ArrayType& array =
        builder.array_types()[builder.types()[receiver].as_array()];
    if (expected != nullptr) {
      return unify(*expected, array.element, node.span, "index");
    }
    return array.element;
  }

  void check_cond(u32 module, ast::CondIdx cond, bool& binds) {
    const ast::Cond& node = ast.conds[cond];
    binds = false;
    if (node.is_pattern) {
      const ir::TypeIdx init = check_expr(module, node.init, nullptr);
      scopes.emplace_back();
      binds = true;
      bind_pattern(module, node.pattern, init);
      if (verify_comp_known && !comp_checked_in_scope(node.init) &&
          !expr_comp_known(module, node.init)) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                     ast.exprs[node.init].span,
                     "comp condition initializer is not comp-known");
        (void)index;
      }
      return;
    }
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    const ir::TypeIdx actual = check_expr(module, node.value, &boolean);
    unify(boolean, actual, ast.exprs[node.value].span, "condition");
    if (verify_comp_known && !comp_checked_in_scope(node.value) &&
        !expr_comp_known(module, node.value)) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                                 ast.exprs[node.value].span,
                                 "comp condition is not comp-known");
      (void)index;
    }
  }

  ir::TypeIdx check_if(u32 module,
                       ast::ExprIdx expr,
                       const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    bool binds = false;
    check_cond(module, node.payload.get<ast::ExprIf>().cond, binds);
    const ir::TypeIdx then = check_block(
        module, node.payload.get<ast::ExprIf>().then_block, expected);
    if (binds) {
      scopes.pop_back();
    }
    if (!node.payload.get<ast::ExprIf>().else_block.is_valid()) {
      if (!is_void(then) && !is_error(then) && !is_never(then)) {
        const u32 index = bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                                   node.span, "if without else yields '()'");
        (void)index;
      }
      const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
      if (expected != nullptr) {
        return unify(*expected, unit, node.span, "if");
      }
      return unit;
    }
    const ir::TypeIdx otherwise = check_block(
        module, node.payload.get<ast::ExprIf>().else_block, expected);
    return unify(then, otherwise, node.span, "if branches");
  }

  // Exhaustiveness over the plan's bounded scope: bool and enum
  // variants by enumeration, integer matches by wildcard (literal
  // ranges cannot cover a full integer type), tuples and structs by
  // wildcard or a matching constructor pattern.
  void check_exhaustive(u32 module,
                        ir::TypeIdx scrutinee,
                        std::span<const ast::ExprMatchArm> arms,
                        diag::Span span) {
    bool wildcard = false;
    std::vector<bool> covered_bool{false, false};
    for (const ast::ExprMatchArm& arm : arms) {
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
      const ast::ItemNode* decl = nullptr;
      for (NominalEntry& entry : nominals) {
        if (!entry.complete || entry.type.idx != scrutinee.idx) {
          continue;
        }
        const ast::ItemNode& candidate = ast.items[entry.item];
        if (candidate.kind != ast::ItemKind::Enum) {
          continue;
        }
        decl = &candidate;
        break;
      }
      if (decl == nullptr) {
        for (const BlessedEntry& entry : blessed) {
          if (entry.type.idx != scrutinee.idx) {
            continue;
          }
          // Blessed constructors cover by side: Ok/Some is variant 0,
          // Err/None is variant 1. Unconditional patterns cover both.
          bool covered[2] = {false, false};
          for (const ast::ExprMatchArm& arm : arms) {
            mark_blessed_covered(module, arm.pattern, covered);
          }
          if (covered[0] && covered[1]) {
            return;
          }
          const char* missing = covered[0] ? (entry.is_result ? "Err" : "None")
                                           : (entry.is_result ? "Ok" : "Some");
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                       "non-exhaustive match: '{}' not covered", missing);
          (void)index;
          return;
        }
        return;
      }
      std::vector<bool> covered(static_cast<usize>(enum_type.variants.size()),
                                false);
      const std::span<const ast::ItemEnumVariant> variants =
          decl->payload.get<ast::ItemEnum>().variants;
      for (const ast::ExprMatchArm& arm : arms) {
        mark_variant_covered(arm.pattern, variants, covered);
      }
      for (usize i = 0; i < covered.size(); ++i) {
        if (!covered[i]) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
              "non-exhaustive match: '{}' not covered", variants[i].name.name);
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
      for (const ast::ExprMatchArm& arm : arms) {
        if (ast.patterns[arm.pattern].kind == ast::PatternKind::Tuple &&
            !ast.patterns[arm.pattern].payload.tuple.path.is_valid()) {
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
      for (const ast::ExprMatchArm& arm : arms) {
        if (ast.patterns[arm.pattern].kind == ast::PatternKind::Struct) {
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

  bool pattern_is_wildcard(ast::PatternIdx pattern) const {
    switch (ast.patterns[pattern].kind) {
      case ast::PatternKind::Wildcard: return true;
      case ast::PatternKind::Ident:
      case ast::PatternKind::MutIdent: return true;
      case ast::PatternKind::Or: {
        for (ast::PatternIdx alt :
             ast.patterns[pattern].payload.or_pat.alternatives) {
          if (pattern_is_wildcard(alt)) {
            return true;
          }
        }
        return false;
      }
      default: return false;
    }
  }

  void collect_bool_literals(ast::PatternIdx pattern,
                             std::vector<bool>& covered) const {
    switch (ast.patterns[pattern].kind) {
      case ast::PatternKind::Literal: {
        const ast::Literal& value =
            ast.literals[ast.patterns[pattern].payload.literal.value];
        if (value.kind == ast::LiteralKind::Bool) {
          covered[value.spelling == "true" ? 0 : 1] = true;
        }
        return;
      }
      case ast::PatternKind::Or: {
        for (ast::PatternIdx alt :
             ast.patterns[pattern].payload.or_pat.alternatives) {
          collect_bool_literals(alt, covered);
        }
        return;
      }
      default: return;
    }
  }

  void mark_variant_covered(ast::PatternIdx pattern,
                            std::span<const ast::ItemEnumVariant> variants,
                            std::vector<bool>& covered) {
    const ast::PatternNode& node = ast.patterns[pattern];
    switch (node.kind) {
      case ast::PatternKind::Ident: {
        for (usize i = 0; i < covered.size(); ++i) {
          if (variants[i].name.name == node.payload.ident.name.name &&
              variants[i].fields.empty()) {
            covered[i] = true;
            return;
          }
        }
        return;
      }
      case ast::PatternKind::Tuple: {
        if (!node.payload.tuple.path.is_valid() ||
            ast.paths[node.payload.tuple.path].segments.empty()) {
          return;
        }
        const std::string_view name =
            ast.paths[node.payload.tuple.path].segments.back().name;
        for (usize i = 0; i < covered.size(); ++i) {
          if (variants[i].name.name == name) {
            covered[i] = true;
            return;
          }
        }
        return;
      }
      case ast::PatternKind::Or: {
        for (ast::PatternIdx alt : node.payload.or_pat.alternatives) {
          mark_variant_covered(alt, variants, covered);
        }
        return;
      }
      default: return;
    }
  }

  void mark_blessed_covered(u32 module,
                            ast::PatternIdx pattern,
                            bool covered[2]) {
    const ast::PatternNode& node = ast.patterns[pattern];
    switch (node.kind) {
      case ast::PatternKind::Wildcard:
      case ast::PatternKind::Ident:
      case ast::PatternKind::MutIdent: return;
      case ast::PatternKind::Tuple: {
        if (!node.payload.tuple.path.is_valid()) {
          return;
        }
        PathValue resolved;
        if (!resolve_variant_path(module, node.payload.tuple.path, resolved)) {
          return;
        }
        if (resolved.kind != PathValue::Kind::BlessedCtor) {
          return;
        }
        covered[resolved.blessed_first ? 0 : 1] = true;
        return;
      }
      case ast::PatternKind::Or: {
        for (ast::PatternIdx alt : node.payload.or_pat.alternatives) {
          mark_blessed_covered(module, alt, covered);
        }
        return;
      }
      default: return;
    }
  }

  ir::TypeIdx check_match(u32 module,
                          ast::ExprIdx expr,
                          const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    const ast::ExprIdx scrutinee_expr =
        node.payload.get<ast::ExprMatch>().scrutinee;
    const ir::TypeIdx scrutinee = check_expr(module, scrutinee_expr, nullptr);
    if (verify_comp_known && !comp_checked_in_scope(scrutinee_expr) &&
        !expr_comp_known(module, scrutinee_expr)) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                                 ast.exprs[scrutinee_expr].span,
                                 "comp match scrutinee is not comp-known");
      (void)index;
    }
    ir::TypeIdx result = error_type();
    bool first = true;
    for (const ast::ExprMatchArm& arm :
         node.payload.get<ast::ExprMatch>().arms) {
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
        result = unify(result, body, ast.exprs[arm.body].span, "match arms");
      }
    }
    if (!is_error(scrutinee)) {
      check_exhaustive(module, scrutinee,
                       node.payload.get<ast::ExprMatch>().arms, node.span);
    }
    if (expected != nullptr && !first) {
      return unify(*expected, result, node.span, "match");
    }
    return result;
  }

  ir::TypeIdx check_expr(u32 module,
                         ast::ExprIdx expr,
                         const ir::TypeIdx* expected) {
    const ir::TypeIdx type = check_expr_inner(module, expr, expected);
    modules[module].expr_types.emplace_back(expr, type);
    return type;
  }

  ir::TypeIdx check_expr_inner(u32 module,
                               ast::ExprIdx expr,
                               const ir::TypeIdx* expected) {
    const ast::ExprNode& node = ast.exprs[expr];
    switch (node.kind) {
      case ast::ExprKind::Literal: {
        return check_literal(node.payload.get<ast::ExprLiteral>().value,
                             expected);
      }
      case ast::ExprKind::Path: {
        return check_path_expr(module, node.payload.get<ast::ExprPath>().idx,
                               expected, node.span);
      }
      case ast::ExprKind::Struct: {
        return check_struct_expr(module, expr, expected);
      }
      case ast::ExprKind::Tuple: {
        const ast::ExprTuple& tuple = node.payload.get<ast::ExprTuple>();
        if (tuple.elements.empty()) {
          const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
          if (expected != nullptr) {
            return unify(*expected, unit, node.span, "unit");
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
        elements.reserve(tuple.elements.size());
        for (usize i = 0; i < tuple.elements.size(); ++i) {
          const ir::TypeIdx* element_expected = nullptr;
          ir::TypeIdx element_type = error_type();
          if (expected_tuple != nullptr &&
              expected_tuple->elements.size() == tuple.elements.size()) {
            element_type = expected_tuple->elements[i];
            element_expected = &element_type;
          }
          elements.push_back(
              check_expr(module, tuple.elements[i], element_expected));
        }
        ir::TypeSeq seq;
        for (ir::TypeIdx element : elements) {
          seq.push(builder.ref_type(element));
        }
        const ir::TypeIdx type = builder.tuple_type(seq.finish());
        if (expected != nullptr) {
          return unify(*expected, type, node.span, "tuple");
        }
        return type;
      }
      case ast::ExprKind::Unary: {
        const ir::TypeIdx inner = check_expr(
            module, node.payload.get<ast::ExprUnary>().inner, nullptr);
        if (is_error(inner)) {
          return error_type();
        }
        const ir::TypeTag tag = tag_of(inner);
        switch (node.payload.get<ast::ExprUnary>().op) {
          case ast::UnaryOp::Neg:
            if (is_integer_tag(tag) || is_float_tag(tag)) {
              if (expected != nullptr) {
                return unify(*expected, inner, node.span, "negation");
              }
              return inner;
            }
            break;
          case ast::UnaryOp::Not:
            if (tag == ir::TypeTag::I1) {
              if (expected != nullptr) {
                return unify(*expected, inner, node.span, "not");
              }
              return inner;
            }
            break;
          case ast::UnaryOp::BitNot:
            if (is_integer_tag(tag)) {
              if (expected != nullptr) {
                return unify(*expected, inner, node.span, "bitwise not");
              }
              return inner;
            }
            break;
        }
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                     node.span, "invalid unary operand '{}'", pretty_tag(tag));
        (void)index;
        return error_type();
      }
      case ast::ExprKind::Borrow: {
        // Place-ness is a borrow-checking concern; here the
        // inner type only determines the reference shape.
        const ir::TypeIdx pointee = check_expr(
            module, node.payload.get<ast::ExprBorrow>().inner, nullptr);
        if (is_error(pointee)) {
          return error_type();
        }
        const ir::TypeIdx type = builder.reference_type(
            pointee, node.payload.get<ast::ExprBorrow>().is_mut);
        if (expected != nullptr) {
          return unify(*expected, type, node.span, "borrow");
        }
        return type;
      }
      case ast::ExprKind::Binary: {
        if (node.payload.get<ast::ExprBinary>().op == ast::BinaryOp::And ||
            node.payload.get<ast::ExprBinary>().op == ast::BinaryOp::Or) {
          const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
          const ir::TypeIdx left = check_expr(
              module, node.payload.get<ast::ExprBinary>().lhs, &boolean);
          const ir::TypeIdx right = check_expr(
              module, node.payload.get<ast::ExprBinary>().rhs, &boolean);
          unify(boolean, left,
                ast.exprs[node.payload.get<ast::ExprBinary>().lhs].span,
                "logical operand");
          unify(boolean, right,
                ast.exprs[node.payload.get<ast::ExprBinary>().rhs].span,
                "logical operand");
          if (expected != nullptr) {
            return unify(*expected, boolean, node.span, "logical");
          }
          return boolean;
        }
        const ir::TypeIdx operands = check_binary_operands(
            module, node.payload.get<ast::ExprBinary>().lhs,
            node.payload.get<ast::ExprBinary>().rhs, node.span, "binary");
        if (is_error(operands)) {
          return error_type();
        }
        const ir::TypeTag tag = tag_of(operands);
        switch (node.payload.get<ast::ExprBinary>().op) {
          case ast::BinaryOp::Eq:
          case ast::BinaryOp::NotEq: {
            const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
            if (expected != nullptr) {
              return unify(*expected, boolean, node.span, "comparison");
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
                return unify(*expected, boolean, node.span, "comparison");
              }
              return boolean;
            }
            break;
          default:
            if (is_integer_tag(tag) || is_float_tag(tag)) {
              if (expected != nullptr) {
                return unify(*expected, operands, node.span, "arithmetic");
              }
              return operands;
            }
            break;
        }
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                     node.span, "invalid binary operand '{}'", pretty_tag(tag));
        (void)index;
        return error_type();
      }
      case ast::ExprKind::Cast: {
        return check_cast(module, expr, expected);
      }
      case ast::ExprKind::Call: {
        return check_call(module, expr, expected);
      }
      case ast::ExprKind::MethodCall: {
        return check_method_call(module, expr, expected);
      }
      case ast::ExprKind::Field: {
        return check_field(module, expr, expected);
      }
      case ast::ExprKind::Index: {
        return check_index(module, expr, expected);
      }
      case ast::ExprKind::Question: {
        return check_question(module, expr, expected);
      }
      case ast::ExprKind::If: {
        return check_if(module, expr, expected);
      }
      case ast::ExprKind::Match: {
        return check_match(module, expr, expected);
      }
      case ast::ExprKind::Loop: {
        const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
        ++loop_depth;
        check_block(module, node.payload.get<ast::ExprLoop>().body, &unit);
        --loop_depth;
        if (expected != nullptr) {
          return unify(*expected, unit, node.span, "loop");
        }
        return unit;
      }
      case ast::ExprKind::While: {
        bool binds = false;
        check_cond(module, node.payload.get<ast::ExprWhile>().cond, binds);
        const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
        ++loop_depth;
        check_block(module, node.payload.get<ast::ExprWhile>().body, &unit);
        --loop_depth;
        if (binds) {
          scopes.pop_back();
        }
        if (expected != nullptr) {
          return unify(*expected, unit, node.span, "while");
        }
        return unit;
      }
      case ast::ExprKind::Block: {
        const ast::ExprBlock& block = node.payload.get<ast::ExprBlock>();
        if (!block.is_comp) {
          return check_block(module, block.block, expected);
        }
        ++comp_depth;
        const bool was_verifying = verify_comp_known;
        verify_comp_known = true;
        const ir::TypeIdx type = check_block(module, block.block, expected);
        verify_comp_known = was_verifying;
        --comp_depth;
        return type;
      }
      case ast::ExprKind::Return: {
        if (comp_depth > 0) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, node.span,
                       "'ret' must not cross a comp block boundary");
          (void)index;
          return error_type();
        }
        if (!in_fn) {
          const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadReturn,
                                     node.span, "'ret' outside of a function");
          (void)index;
          return error_type();
        }
        if (!node.payload.get<ast::ExprReturn>().value.is_valid()) {
          unify(fn_ret, builder.primitive(ir::TypeTag::Void), node.span,
                "return");
        } else {
          const ir::TypeIdx value = check_expr(
              module, node.payload.get<ast::ExprReturn>().value, &fn_ret);
          unify(fn_ret, value,
                ast.exprs[node.payload.get<ast::ExprReturn>().value].span,
                "return");
        }
        return builder.never_type();
      }
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue: {
        if (loop_depth == 0) {
          if (node.kind == ast::ExprKind::Break) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerBreakOutsideLoop,
                         node.span, "'break' outside of a loop");
            (void)index;
          } else {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerBreakOutsideLoop,
                         node.span, "'continue' outside of a loop");
            (void)index;
          }
          return error_type();
        }
        return builder.never_type();
      }
      case ast::ExprKind::Range: {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerUnsupportedExpr, node.span,
                     "range expressions arrive post-MVP");
        (void)index;
        return error_type();
      }
    }
  }

  ir::TypeIdx check_block(u32 module,
                          ast::BlockIdx block,
                          const ir::TypeIdx* expected) {
    const ast::Block& node = ast.blocks[block];
    scopes.emplace_back();
    for (ast::StmtIdx stmt : node.statements) {
      check_stmt(module, stmt);
    }
    ir::TypeIdx result = builder.primitive(ir::TypeTag::Void);
    if (node.value.is_valid()) {
      result = check_expr(module, node.value, expected);
      if (expected != nullptr) {
        result = unify(*expected, result, ast.exprs[node.value].span, "block");
      }
      if (verify_comp_known && !expr_comp_known(module, node.value)) {
        const u32 index = bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                                   ast.exprs[node.value].span,
                                   "comp block value is not comp-known");
        (void)index;
      }
    } else if (expected != nullptr) {
      result = unify(*expected, result, node.span, "block");
    }
    scopes.pop_back();
    return result;
  }

  ir::TypeIdx check_place(u32 module, ast::ExprIdx place) {
    const ast::ExprNode& node = ast.exprs[place];
    switch (node.kind) {
      case ast::ExprKind::Path: {
        const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
        PathValue resolved;
        if (!resolve_value_path(module, path, resolved)) {
          return error_type();
        }
        if (resolved.kind != PathValue::Kind::Local) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerBadAssignment, node.span,
                       "cannot assign to this place");
          (void)index;
          return error_type();
        }
        // Locals shadow everything, but the resolution above may have
        // found the name through another namespace; confirm mutability
        // through the scope entry.
        const Local* local = lookup_local(ast.paths[path].segments.back().name);
        if (local == nullptr || !local->is_mut) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerBadAssignment, node.span,
                       "cannot assign to an immutable binding");
          (void)index;
          return error_type();
        }
        if (verify_comp_known && !local->comp_known) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown, node.span,
                       "comp assignment place is not comp-known");
          (void)index;
          return error_type();
        }
        return local->type;
      }
      case ast::ExprKind::Field: {
        const ir::TypeIdx receiver =
            check_place(module, node.payload.get<ast::ExprField>().receiver);
        if (is_error(receiver)) {
          return error_type();
        }
        if (tag_of(receiver) != ir::TypeTag::Struct) {
          return error_type();
        }
        return check_field(module, place, nullptr);
      }
      case ast::ExprKind::Index: {
        const ir::TypeIdx receiver =
            check_place(module, node.payload.get<ast::ExprIndex>().receiver);
        if (is_error(receiver)) {
          return error_type();
        }
        return check_index(module, place, nullptr);
      }
      default: {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerBadAssignment, node.span,
                     "cannot assign to this place");
        (void)index;
        return error_type();
      }
    }
  }

  void check_stmt(u32 module, ast::StmtIdx stmt) {
    const ast::StmtNode& node = ast.stmts[stmt];
    switch (node.kind) {
      case ast::StmtKind::Decl: {
        const ast::StmtDecl& decl = node.payload.get<ast::StmtDecl>();
        const ir::TypeIdx* expected = nullptr;
        ir::TypeIdx ascribed = error_type();
        if (decl.type.is_valid()) {
          ascribed = resolve_type(module, decl.type, nullptr);
          expected = &ascribed;
        }
        bool entered_comp = false;
        if (decl.is_comp) {
          ++comp_depth;
          entered_comp = true;
        }
        const ir::TypeIdx init = check_expr(module, decl.init, expected);
        if (expected != nullptr) {
          unify(*expected, init, ast.exprs[decl.init].span, "declaration");
        }
        if ((decl.is_comp || verify_comp_known) &&
            !comp_checked_in_scope(decl.init) &&
            !expr_comp_known(module, decl.init)) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                       ast.exprs[decl.init].span,
                       "comp declaration initializer is not comp-known");
          (void)index;
        }
        bind_comp_known = decl.is_comp;
        const bool refutable = bind_pattern(module, decl.pattern, init);
        bind_comp_known = false;
        if (entered_comp) {
          --comp_depth;
        }
        if (refutable) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerRefutableLet,
                       ast.patterns[decl.pattern].span,
                       "refutable pattern in declaration; use match");
          (void)index;
        }
        return;
      }
      case ast::StmtKind::Reassign: {
        const ast::StmtReassign& reassign =
            node.payload.get<ast::StmtReassign>();
        if (comp_depth == 0 &&
            ast.exprs[reassign.place].kind == ast::ExprKind::Path) {
          const ast::PathIdx path =
              ast.exprs[reassign.place].payload.get<ast::ExprPath>().idx;
          const std::span<const ast::Ident> segments = ast.paths[path].segments;
          if (segments.size() == 1) {
            if (const Local* local = lookup_local(segments[0].name)) {
              if (local->comp_known) {
                const u32 index = bag.emit(
                    diag::Severity::Error, kAnalyzerInvalidComp, node.span,
                    "cannot reassign a comp binding at runtime");
                (void)index;
                return;
              }
            }
          }
        }
        const ir::TypeIdx place = check_place(module, reassign.place);
        const ir::TypeIdx value = check_expr(
            module, node.payload.get<ast::StmtReassign>().value, &place);
        if (verify_comp_known &&
            !comp_checked_in_scope(
                node.payload.get<ast::StmtReassign>().value) &&
            !expr_comp_known(module,
                             node.payload.get<ast::StmtReassign>().value)) {
          const u32 index = bag.emit(
              diag::Severity::Error, kAnalyzerNotCompKnown,
              ast.exprs[node.payload.get<ast::StmtReassign>().value].span,
              "comp assignment value is not comp-known");
          (void)index;
        }
        if (node.payload.get<ast::StmtReassign>().compound) {
          const ir::TypeTag tag = tag_of(place);
          if (!is_integer_tag(tag) && !is_float_tag(tag)) {
            const u32 index = bag.emit(
                diag::Severity::Error, kAnalyzerInvalidOperation, node.span,
                "compound assignment needs a numeric place");
            (void)index;
            return;
          }
        }
        unify(place, value,
              ast.exprs[node.payload.get<ast::StmtReassign>().value].span,
              "assignment");
        return;
      }
      case ast::StmtKind::Expr: {
        const ir::TypeIdx type = check_expr(
            module, node.payload.get<ast::StmtExpr>().value, nullptr);
        const diag::Span span =
            ast.exprs[node.payload.get<ast::StmtExpr>().value].span;
        if (is_void(type) || is_error(type) || is_never(type)) {
          return;
        }
        if (is_must_use(type)) {
          const u32 index = bag.emit(
              diag::Severity::Warning, kAnalyzerMustUse, span,
              "unused Result/Option value; bind or discard it explicitly");
          (void)index;
          return;
        }
        const u32 index = bag.emit(
            diag::Severity::Warning, kAnalyzerMustUse, span,
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

  void check_fn(u32 module, ast::ItemIdx fn, const ir::TypeIdx* self) {
    const ast::ItemNode& node = ast.items[fn];
    fn_ret = builder.primitive(ir::TypeTag::Void);
    if (node.payload.get<ast::ItemFn>().return_type.is_valid()) {
      fn_ret = resolve_type(module, node.payload.get<ast::ItemFn>().return_type,
                            self);
    }
    in_fn = true;
    scopes.emplace_back();
    loop_depth = 0;
    for (const ast::ItemFnParam& param :
         node.payload.get<ast::ItemFn>().params) {
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

  void check_main(u32 module, ast::ItemIdx fn) {
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
    if (const BlessedEntry* entry = blessed_find(ret)) {
      if (entry->is_result && is_void(entry->args[0])) {
        return;
      }
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerBadReturn, node.span,
                 "'main' must return '()', 'i32', or 'Result<(), E>'");
    (void)index;
  }

  void check_bodies() {
    for (u32 m = 0; m < static_cast<u32>(tree.modules.size()); ++m) {
      for (ast::ItemIdx item : tree.modules[m]->items) {
        const ast::ItemNode& node = ast.items[item];
        switch (node.kind) {
          case ast::ItemKind::Fn: {
            check_fn(m, item, nullptr);
            if (m == tree.root &&
                node.payload.get<ast::ItemFn>().name.name == "main") {
              check_main(m, item);
            }
            break;
          }
          case ast::ItemKind::Impl: {
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
                  self = intern_nominal(*entry);
                  self_ptr = &self;
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

  CheckedModule empty_module(u32 module) {
    CheckedModule checked;
    checked.module = module;
    return checked;
  }
};

}  // namespace

diag::Fallible<CheckedPackage> check_package(const ModuleTree& tree,
                                             ir::PointerWidth width,
                                             ast::AstArena& ast,
                                             diag::DiagBag& bag) {
  Checker checker{tree, width, ast, bag};
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
