// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/summary.h"

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/deserialize.h"
#include "ir/function.h"
#include "ir/storage.h"
#include "ir/summary.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace analyzer {

namespace {

// Where a nominal row's declaration is spelled: the module path its row
// names, dotted from the package root, and the name it goes by.
struct NominalSite {
  std::string_view path;
  std::string_view name;
};

// What one spelling pass reads and writes.
struct SpellContext {
  const ir::Storage& storage;
  ast::AstArena& ast;
  // The artifact's interner, which every row's ids resolve in.
  const ir::SymbolTable& artifact_strings;
  // The run's interner; every spelled name is copied into it, so
  // nothing borrows the artifact.
  ir::SymbolTable& strings;
  // Every nominal row's declaration, by the type the row names.
  const std::unordered_map<u32, NominalSite>& nominals;
  // The same rows by the name their shape carries: a reference may name
  // a copy of a declaration rather than the declaration itself.
  const std::unordered_map<u32, std::vector<u32>>& nominals_by_name;
};

// How many pairs one shape walk may hold before it answers false: a
// declaration that contains a reference to itself repeats a pair, and
// the walk treats a repeated pair as still matching, so the bound only
// guards against a table that keeps producing new ones.
constexpr usize MAX_SHAPE_WALK = 4096;

// The name a nominal node's shape carries, or nothing for a node that
// is not a nominal.
std::optional<u32> shape_name(const ir::Storage& storage, ir::TypeIdx type) {
  if (!type.is_valid() || type.idx >= storage.types().size()) {
    return std::nullopt;
  }
  const ir::TypeNode& node = storage.types()[type];
  if (node.tag == ir::TypeTag::Struct) {
    const ir::StructTypeIdx shape = node.as_struct();
    if (shape.idx >= storage.struct_types().size()) {
      return std::nullopt;
    }
    return storage.struct_types()[shape].name.offset;
  }
  if (node.tag == ir::TypeTag::Enum) {
    const ir::EnumTypeIdx shape = node.as_enum();
    if (shape.idx >= storage.enum_types().size()) {
      return std::nullopt;
    }
    return storage.enum_types()[shape].name.offset;
  }
  return std::nullopt;
}

// Whether two type nodes describe the same shape, down to the names and
// payloads a nominal's layout reads. A type table holds more than one
// node over one declaration - a field or a parameter holds a copy of
// the nominal it names - so a reference matches a row by the shape they
// repeat. Identical shapes lay out alike, so a match is enough to spell
// against; what the walk cannot be sure of answers false, which leaves
// the row out instead of spelling a shape it does not know.
bool shapes_match(const ir::Storage& storage,
                  ir::TypeIdx left,
                  ir::TypeIdx right,
                  std::vector<std::pair<u32, u32>>& seen) {
  if (left.idx == right.idx) {
    return true;
  }
  for (const std::pair<u32, u32>& pair : seen) {
    if (pair.first == left.idx && pair.second == right.idx) {
      // The pair is already being walked: a reference back into it
      // matches what it is being compared against.
      return true;
    }
  }
  if (!left.is_valid() || !right.is_valid() ||
      left.idx >= storage.types().size() ||
      right.idx >= storage.types().size() || seen.size() >= MAX_SHAPE_WALK) {
    return false;
  }
  const usize mark = seen.size();
  seen.emplace_back(left.idx, right.idx);
  // A failure drops everything this frame added, so one failed branch
  // leaves no pair behind for the next to read as matching.
  const auto fail = [&] {
    seen.resize(mark);
    return false;
  };
  const auto match = [&](ir::TypeIdx a, ir::TypeIdx b) {
    return shapes_match(storage, a, b, seen);
  };
  const ir::TypeNode& a = storage.types()[left];
  const ir::TypeNode& b = storage.types()[right];
  if (a.tag != b.tag) {
    return fail();
  }
  switch (a.tag) {
    case ir::TypeTag::Struct: {
      if (a.as_struct().idx >= storage.struct_types().size() ||
          b.as_struct().idx >= storage.struct_types().size()) {
        return fail();
      }
      const ir::StructType& left_shape = storage.struct_types()[a.as_struct()];
      const ir::StructType& right_shape = storage.struct_types()[b.as_struct()];
      if (left_shape.name != right_shape.name ||
          left_shape.fields.size() != right_shape.fields.size() ||
          left_shape.params.size() != right_shape.params.size()) {
        return fail();
      }
      for (usize i = 0; i < left_shape.fields.size(); ++i) {
        if (!match(left_shape.fields[i], right_shape.fields[i])) {
          return fail();
        }
      }
      for (usize i = 0; i < left_shape.params.size(); ++i) {
        if (!match(left_shape.params[i], right_shape.params[i])) {
          return fail();
        }
      }
      return true;
    }
    case ir::TypeTag::Enum: {
      if (a.as_enum().idx >= storage.enum_types().size() ||
          b.as_enum().idx >= storage.enum_types().size()) {
        return fail();
      }
      const ir::EnumType& left_shape = storage.enum_types()[a.as_enum()];
      const ir::EnumType& right_shape = storage.enum_types()[b.as_enum()];
      if (left_shape.name != right_shape.name ||
          left_shape.variants.size() != right_shape.variants.size() ||
          left_shape.params.size() != right_shape.params.size()) {
        return fail();
      }
      for (usize i = 0; i < left_shape.params.size(); ++i) {
        if (!match(left_shape.params[i], right_shape.params[i])) {
          return fail();
        }
      }
      for (usize i = 0; i < left_shape.variants.size(); ++i) {
        const ir::EnumVariantTypeIdx left_variant = left_shape.variants[i];
        const ir::EnumVariantTypeIdx right_variant = right_shape.variants[i];
        if (left_variant.idx >= storage.enum_variant_types().size() ||
            right_variant.idx >= storage.enum_variant_types().size()) {
          return fail();
        }
        const ir::EnumVariantType& left_payload =
            storage.enum_variant_types()[left_variant];
        const ir::EnumVariantType& right_payload =
            storage.enum_variant_types()[right_variant];
        if (left_payload.name != right_payload.name ||
            left_payload.fields.size() != right_payload.fields.size()) {
          return fail();
        }
        for (usize f = 0; f < left_payload.fields.size(); ++f) {
          if (!match(left_payload.fields[f], right_payload.fields[f])) {
            return fail();
          }
        }
      }
      return true;
    }
    case ir::TypeTag::Ref:
    case ir::TypeTag::MutRef:
    case ir::TypeTag::RawPtr:
    case ir::TypeTag::RawMutPtr: {
      const ir::RefTypeIdx left_shape = a.as_ref();
      const ir::RefTypeIdx right_shape = b.as_ref();
      if (left_shape.idx >= storage.ref_types().size() ||
          right_shape.idx >= storage.ref_types().size()) {
        return fail();
      }
      return match(storage.ref_types()[left_shape].pointee,
                   storage.ref_types()[right_shape].pointee)
                 ? true
                 : fail();
    }
    case ir::TypeTag::Slice: {
      const ir::SliceTypeIdx left_shape = a.as_slice();
      const ir::SliceTypeIdx right_shape = b.as_slice();
      if (left_shape.idx >= storage.slice_types().size() ||
          right_shape.idx >= storage.slice_types().size()) {
        return fail();
      }
      return match(storage.slice_types()[left_shape].element,
                   storage.slice_types()[right_shape].element)
                 ? true
                 : fail();
    }
    case ir::TypeTag::Array: {
      const ir::ArrayTypeIdx left_shape = a.as_array();
      const ir::ArrayTypeIdx right_shape = b.as_array();
      if (left_shape.idx >= storage.array_types().size() ||
          right_shape.idx >= storage.array_types().size()) {
        return fail();
      }
      const ir::ArrayType& left_array = storage.array_types()[left_shape];
      const ir::ArrayType& right_array = storage.array_types()[right_shape];
      if (left_array.count != right_array.count) {
        return fail();
      }
      return match(left_array.element, right_array.element) ? true : fail();
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleTypeIdx left_shape = a.as_tuple();
      const ir::TupleTypeIdx right_shape = b.as_tuple();
      if (left_shape.idx >= storage.tuple_types().size() ||
          right_shape.idx >= storage.tuple_types().size()) {
        return fail();
      }
      const ir::TupleType& left_tuple = storage.tuple_types()[left_shape];
      const ir::TupleType& right_tuple = storage.tuple_types()[right_shape];
      if (left_tuple.elements.size() != right_tuple.elements.size()) {
        return fail();
      }
      for (usize i = 0; i < left_tuple.elements.size(); ++i) {
        if (!match(left_tuple.elements[i], right_tuple.elements[i])) {
          return fail();
        }
      }
      return true;
    }
    case ir::TypeTag::Function: {
      const ir::FuncTypeIdx left_shape = a.as_func();
      const ir::FuncTypeIdx right_shape = b.as_func();
      if (left_shape.idx >= storage.func_types().size() ||
          right_shape.idx >= storage.func_types().size()) {
        return fail();
      }
      const ir::FuncType& left_func = storage.func_types()[left_shape];
      const ir::FuncType& right_func = storage.func_types()[right_shape];
      if (left_func.params.size() != right_func.params.size()) {
        return fail();
      }
      for (usize i = 0; i < left_func.params.size(); ++i) {
        if (!match(left_func.params[i], right_func.params[i])) {
          return fail();
        }
      }
      return match(left_func.ret, right_func.ret) ? true : fail();
    }
    case ir::TypeTag::Const:
      // A value argument has no layout and takes no part in a field's
      // shape; two entries are the same value or the walk does not say.
      if (a.as_const().idx >= storage.const_types().size() ||
          b.as_const().idx >= storage.const_types().size()) {
        return fail();
      }
      return storage.const_types()[a.as_const()].value ==
                     storage.const_types()[b.as_const()].value
                 ? true
                 : fail();
    default:
      // Every other tag carries no payload, so equal tags are the whole
      // shape.
      return true;
  }
}

// The ident one path segment is spelled as.
std::optional<ast::Ident> spell_segment(const SpellContext& ctx,
                                        std::string_view name) {
  const std::optional<str::StringPoolId> copied =
      ctx.strings.intern_copied(name);
  if (!copied.has_value()) {
    return std::nullopt;
  }
  return ast::Ident{ctx.strings.get(*copied), diag::Span{}};
}

// The type path a nominal's declaration is reachable by: `package::`
// opens this package's root from any module in it, and the segments the
// row recorded walk down to the declaration.
std::optional<ast::TypeIdx> spell_nominal_path(const SpellContext& ctx,
                                               const NominalSite& site) {
  ast::AstArena& ast = ctx.ast;
  std::vector<ast::Ident> segments;
  const std::optional<ast::Ident> head = spell_segment(ctx, "package");
  if (!head.has_value()) {
    return std::nullopt;
  }
  segments.push_back(*head);
  std::string_view rest = site.path;
  while (!rest.empty()) {
    const usize dot = rest.find('.');
    const std::optional<ast::Ident> spelled =
        spell_segment(ctx, rest.substr(0, dot));
    if (!spelled.has_value()) {
      return std::nullopt;
    }
    segments.push_back(*spelled);
    rest = dot == std::string_view::npos ? std::string_view{}
                                         : rest.substr(dot + 1);
  }
  const std::optional<ast::Ident> last = spell_segment(ctx, site.name);
  if (!last.has_value()) {
    return std::nullopt;
  }
  segments.push_back(*last);
  ast::TypeNode made;
  made.kind = ast::TypeKind::Path;
  const ast::PathIdx path = ast.paths.push_back(
      ast::Path{ast::copy_to_arena(ast.spans, segments), diag::Span{}});
  made.payload.set(ast::TypePath{.path = path, .args = {}});
  return ast.types.push_back(made);
}

// Spells one type of the summary's tables into the syntax arena; invalid
// when the syntax cannot carry it. A nominal is spelled as the path its
// declaration is reachable by - `package::` and the module it sits in -
// and a raw pointer or constant has no spelling a dependent can use.
ast::TypeIdx spell_type(const SpellContext& ctx,
                        ir::TypeIdx type,
                        bool allow_copies) {
  const ir::Storage& storage = ctx.storage;
  ast::AstArena& ast = ctx.ast;
  if (!type.is_valid() || type.idx >= storage.types().size()) {
    return ast::TypeIdx::invalid();
  }
  ast::TypeNode made;
  const auto primitive = [&](ast::PrimitiveKind kind) {
    made.kind = ast::TypeKind::Primitive;
    made.payload.set(ast::TypePrimitive{.primitive = kind});
    return ast.types.push_back(made);
  };
  const ir::TypeNode& node = storage.types()[type];
  switch (node.tag) {
    case ir::TypeTag::Void:
      made.kind = ast::TypeKind::Unit;
      return ast.types.push_back(made);
    case ir::TypeTag::Never:
      made.kind = ast::TypeKind::Never;
      return ast.types.push_back(made);
    case ir::TypeTag::Str:
      made.kind = ast::TypeKind::Str;
      return ast.types.push_back(made);
    case ir::TypeTag::I1: return primitive(ast::PrimitiveKind::Bool);
    case ir::TypeTag::I8: return primitive(ast::PrimitiveKind::I8);
    case ir::TypeTag::I16: return primitive(ast::PrimitiveKind::I16);
    case ir::TypeTag::I32: return primitive(ast::PrimitiveKind::I32);
    case ir::TypeTag::I64: return primitive(ast::PrimitiveKind::I64);
    case ir::TypeTag::U8: return primitive(ast::PrimitiveKind::U8);
    case ir::TypeTag::U16: return primitive(ast::PrimitiveKind::U16);
    case ir::TypeTag::U32: return primitive(ast::PrimitiveKind::U32);
    case ir::TypeTag::U64: return primitive(ast::PrimitiveKind::U64);
    case ir::TypeTag::F32: return primitive(ast::PrimitiveKind::F32);
    case ir::TypeTag::F64: return primitive(ast::PrimitiveKind::F64);
    case ir::TypeTag::Ref:
    case ir::TypeTag::MutRef: {
      const ir::RefType& shape = storage.ref_types()[node.as_ref()];
      const ast::TypeIdx inner = spell_type(ctx, shape.pointee, allow_copies);
      if (!inner.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      made.kind = ast::TypeKind::Ref;
      made.payload.set(ast::TypeRef{.is_mut = node.tag == ir::TypeTag::MutRef,
                                    .inner = inner});
      return ast.types.push_back(made);
    }
    case ir::TypeTag::Slice: {
      const ir::SliceType& shape = storage.slice_types()[node.as_slice()];
      const ast::TypeIdx element = spell_type(ctx, shape.element, allow_copies);
      if (!element.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      made.kind = ast::TypeKind::Slice;
      made.payload.set(ast::TypeSlice{.element = element});
      return ast.types.push_back(made);
    }
    case ir::TypeTag::Array: {
      const ir::ArrayType& shape = storage.array_types()[node.as_array()];
      const ast::TypeIdx element = spell_type(ctx, shape.element, allow_copies);
      if (!element.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      made.kind = ast::TypeKind::Array;
      made.payload.set(ast::TypeArray{.element = element,
                                      .count = shape.count,
                                      .length_path = ast::PathIdx::invalid()});
      return ast.types.push_back(made);
    }
    case ir::TypeTag::Tuple: {
      const ir::TupleType& shape = storage.tuple_types()[node.as_tuple()];
      std::vector<ast::TypeIdx> elements;
      elements.reserve(shape.elements.size());
      for (ir::TypeIdx element : shape.elements) {
        const ast::TypeIdx spelled = spell_type(ctx, element, allow_copies);
        if (!spelled.is_valid()) {
          return ast::TypeIdx::invalid();
        }
        elements.push_back(spelled);
      }
      made.kind = ast::TypeKind::Tuple;
      made.payload.set(
          ast::TypeTuple{.elements = ast::copy_to_arena(ast.spans, elements)});
      return ast.types.push_back(made);
    }
    case ir::TypeTag::Function: {
      const ir::FuncType& shape = storage.func_types()[node.as_func()];
      std::vector<ast::TypeIdx> params;
      params.reserve(shape.params.size());
      for (ir::TypeIdx param : shape.params) {
        const ast::TypeIdx spelled = spell_type(ctx, param, allow_copies);
        if (!spelled.is_valid()) {
          return ast::TypeIdx::invalid();
        }
        params.push_back(spelled);
      }
      const ast::TypeIdx ret = spell_type(ctx, shape.ret, allow_copies);
      if (!ret.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      made.kind = ast::TypeKind::Func;
      made.payload.set(ast::TypeFunc{
          .params = ast::copy_to_arena(ast.spans, params), .ret = ret});
      return ast.types.push_back(made);
    }
    case ir::TypeTag::Struct:
    case ir::TypeTag::Enum: {
      const auto site = ctx.nominals.find(type.idx);
      if (site != ctx.nominals.end()) {
        return spell_nominal_path(ctx, site->second)
            .value_or(ast::TypeIdx::invalid());
      }
      // A declaration's shape may name a copy of another declaration
      // rather than the declaration itself, as a field holds one; the
      // row whose shape it repeats is the one to spell, and it has to be
      // the only one, because two rows whose shapes both match leave the
      // choice to the walk, which does not make it. A signature takes a
      // value instead, and spelling it as another row's declaration
      // would pass what the caller has where the callee takes something
      // else, so only a declaration reads through a copy.
      if (!allow_copies) {
        return ast::TypeIdx::invalid();
      }
      const std::optional<u32> name = shape_name(storage, type);
      if (!name.has_value()) {
        return ast::TypeIdx::invalid();
      }
      const auto candidates = ctx.nominals_by_name.find(*name);
      if (candidates == ctx.nominals_by_name.end()) {
        return ast::TypeIdx::invalid();
      }
      const NominalSite* matched = nullptr;
      for (const u32 candidate : candidates->second) {
        std::vector<std::pair<u32, u32>> seen;
        if (!shapes_match(storage, type, ir::TypeIdx(candidate), seen)) {
          continue;
        }
        const auto found = ctx.nominals.find(candidate);
        if (found == ctx.nominals.end()) {
          continue;
        }
        if (matched != nullptr) {
          return ast::TypeIdx::invalid();
        }
        matched = &found->second;
      }
      if (matched == nullptr) {
        return ast::TypeIdx::invalid();
      }
      return spell_nominal_path(ctx, *matched)
          .value_or(ast::TypeIdx::invalid());
    }
    default: return ast::TypeIdx::invalid();
  }
}

// Spells one free function of the summary into an item: a wildcard per
// parameter, because a summary carries no pattern names, and no body,
// because the package's own artifact holds the code.
bool spell_function(const SpellContext& ctx,
                    std::string_view name,
                    ir::FunctionIdx function_index,
                    ast::ItemIdx& item_out) {
  const ir::Storage& storage = ctx.storage;
  ast::AstArena& ast = ctx.ast;
  if (function_index.idx >= storage.functions().size()) {
    return false;
  }
  const ir::FunctionMeta& meta = storage.functions()[function_index].meta;
  if (meta.kind != ir::SymbolKind::Free || !meta.generics.empty()) {
    return false;
  }
  std::vector<ast::ItemFnParam> params;
  params.reserve(meta.param_types.size());
  for (ir::TypeIdx param : meta.param_types) {
    const ast::TypeIdx spelled = spell_type(ctx, param, false);
    if (!spelled.is_valid()) {
      return false;
    }
    ast::PatternNode pattern;
    pattern.kind = ast::PatternKind::Wildcard;
    const ast::PatternIdx pattern_index = ast.patterns.push_back(pattern);
    params.push_back(
        ast::ItemFnParam{.pattern = pattern_index, .type = spelled});
  }
  const ast::TypeIdx ret = spell_type(ctx, meta.return_type, false);
  if (!ret.is_valid()) {
    return false;
  }
  ast::ItemFn fn;
  fn.name = ast::Ident{name, diag::Span{}};
  fn.params = ast::copy_to_arena(ast.spans, params);
  fn.return_type = ret;
  ast::ItemNode node;
  node.kind = ast::ItemKind::Fn;
  node.is_pub = true;
  node.payload.set(fn);
  item_out = ast.items.push_back(node);
  return item_out.is_valid();
}

// Spells one nominal row into its declaration: a struct carries the
// field names and visibility the row recorded, an enum carries the
// variant names its table already holds. A dependent can then name the
// type, and the layout it spells is the one the library lays out.
bool spell_declaration(const SpellContext& ctx,
                       const ir::SummaryEntry& row,
                       std::string_view name,
                       ast::ItemIdx& item_out) {
  const ir::Storage& storage = ctx.storage;
  ast::AstArena& ast = ctx.ast;
  if (row.function.idx >= storage.types().size()) {
    return false;
  }
  const ir::TypeNode& type = storage.types()[row.function.idx];
  if (row.kind == ir::SummaryKind::Struct) {
    if (type.tag != ir::TypeTag::Struct ||
        type.as_struct().idx >= storage.struct_types().size()) {
      return false;
    }
    const ir::StructType& shape = storage.struct_types()[type.as_struct()];
    // A field the row does not name is one the declaration would get
    // wrong, so the declaration is left out whole.
    if (row.fields.size() != shape.fields.size()) {
      return false;
    }
    std::vector<ast::ItemStructField> fields;
    fields.reserve(shape.fields.size());
    for (usize i = 0; i < shape.fields.size(); ++i) {
      const ast::TypeIdx spelled = spell_type(ctx, shape.fields[i], true);
      if (!spelled.is_valid()) {
        return false;
      }
      const std::optional<ast::Ident> field_name =
          spell_segment(ctx, ctx.artifact_strings.get(row.fields[i].name));
      if (!field_name.has_value()) {
        return false;
      }
      fields.push_back(ast::ItemStructField{.name = *field_name,
                                            .type = spelled,
                                            .is_pub = row.fields[i].is_pub});
    }
    ast::ItemNode node;
    node.kind = ast::ItemKind::Struct;
    node.is_pub = true;
    node.payload.set(
        ast::ItemStruct{.name = ast::Ident{name, diag::Span{}},
                        .params = {},
                        .fields = ast::copy_to_arena(ast.spans, fields)});
    item_out = ast.items.push_back(node);
    return item_out.is_valid();
  }
  if (type.tag != ir::TypeTag::Enum ||
      type.as_enum().idx >= storage.enum_types().size()) {
    return false;
  }
  const ir::EnumType& shape = storage.enum_types()[type.as_enum()];
  std::vector<ast::ItemEnumVariant> variants;
  variants.reserve(shape.variants.size());
  for (ir::EnumVariantTypeIdx variant_index : shape.variants) {
    if (variant_index.idx >= storage.enum_variant_types().size()) {
      return false;
    }
    const ir::EnumVariantType& variant =
        storage.enum_variant_types()[variant_index];
    std::vector<ast::TypeIdx> payload;
    payload.reserve(variant.fields.size());
    for (ir::TypeIdx field : variant.fields) {
      const ast::TypeIdx spelled = spell_type(ctx, field, true);
      if (!spelled.is_valid()) {
        return false;
      }
      payload.push_back(spelled);
    }
    const std::optional<ast::Ident> variant_name =
        spell_segment(ctx, ctx.artifact_strings.get(variant.name));
    if (!variant_name.has_value()) {
      return false;
    }
    variants.push_back(
        ast::ItemEnumVariant{.name = *variant_name,
                             .fields = ast::copy_to_arena(ast.spans, payload)});
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Enum;
  node.is_pub = true;
  node.payload.set(
      ast::ItemEnum{.name = ast::Ident{name, diag::Span{}},
                    .params = {},
                    .variants = ast::copy_to_arena(ast.spans, variants)});
  item_out = ast.items.push_back(node);
  return item_out.is_valid();
}

}  // namespace

std::string summary_module_name(std::string_view dotted) {
  std::string slash;
  slash.reserve(dotted.size());
  for (const char ch : dotted) {
    const char mapped = ch == ':' ? '/' : ch;
    if (mapped == '/' && !slash.empty() && slash.back() == '/') {
      continue;
    }
    slash.push_back(mapped);
  }
  return slash;
}

SummarySurface spell_summary_surface(const ir::LoadedIr& summary,
                                     std::string_view identity,
                                     ast::AstArena& ast,
                                     ir::SymbolTable& strings) {
  SummarySurface surface;
  surface.identity.assign(identity);
  // Names are copied into the interner, so the surface outlives the
  // artifact it was read from.
  const auto named = [&](str::StringPoolId id,
                         std::string_view fallback) -> std::string_view {
    const std::string_view bytes = summary.strings->get(id);
    const std::optional<str::StringPoolId> copied =
        strings.intern_copied(bytes.empty() ? fallback : bytes);
    if (!copied.has_value()) {
      return std::string_view{};
    }
    return strings.get(*copied);
  };
  const ir::Storage& storage = *summary.storage;
  // The nominals first: a signature may name one, so the map the
  // spelling reads is whole before any item is spelled.
  std::unordered_map<u32, NominalSite> nominals;
  std::unordered_map<u32, std::vector<u32>> nominals_by_name;
  for (const ir::SummaryEntry& row : summary.summary) {
    if (row.kind != ir::SummaryKind::Struct &&
        row.kind != ir::SummaryKind::Enum) {
      continue;
    }
    if (row.needs_body || !row.function.is_valid()) {
      continue;
    }
    nominals.emplace(row.function.idx,
                     NominalSite{named(row.path, ""), named(row.name, "")});
    const std::optional<u32> name =
        shape_name(storage, ir::TypeIdx(row.function.idx));
    if (name.has_value()) {
      nominals_by_name[*name].push_back(row.function.idx);
    }
  }
  const SpellContext context{storage, ast,      *summary.strings,
                             strings, nominals, nominals_by_name};
  std::unordered_map<std::string, std::vector<ast::ItemIdx>> items_by_module;
  std::unordered_map<std::string, std::vector<SummarySymbol>> symbols_by_module;
  std::vector<std::string> order;
  for (const ir::SummaryEntry& row : summary.summary) {
    // What a dependent cannot use yet - an associated function, a
    // method, or a declaration that needs its body - is left for the
    // slices the summary carries them in.
    if (row.needs_body || !row.function.is_valid()) {
      continue;
    }
    if (row.kind != ir::SummaryKind::Free &&
        row.kind != ir::SummaryKind::Struct &&
        row.kind != ir::SummaryKind::Enum) {
      continue;
    }
    const std::string_view name = named(row.name, "");
    const std::string_view path = named(row.path, "");
    // A row without a symbol is one the writer could not name; the
    // source name is the closest a surface can spell.
    const std::string_view symbol = named(row.symbol, name);
    ast::ItemIdx item = ast::ItemIdx::invalid();
    const bool spelled = row.kind == ir::SummaryKind::Free
                             ? spell_function(context, name, row.function, item)
                             : spell_declaration(context, row, name, item);
    if (!spelled) {
      continue;
    }
    std::vector<ast::ItemIdx>& items = items_by_module[std::string(path)];
    if (items.empty()) {
      order.emplace_back(path);
    }
    items.push_back(item);
    symbols_by_module[std::string(path)].push_back(
        SummarySymbol{strings.intern(name), strings.intern(symbol)});
    if (ast.exhausted()) {
      return surface;
    }
  }
  // The items and the paths they belong to are copied into the arena, so
  // nothing here outlives the call.
  for (const std::string& path : order) {
    const std::vector<ast::ItemIdx>& items = items_by_module[path];
    const std::vector<SummarySymbol>& symbols = symbols_by_module[path];
    surface.modules.push_back(
        SummarySurface::Module{path, ast::copy_to_arena(ast.spans, items),
                               ast::copy_to_arena(ast.spans, symbols)});
  }
  return surface;
}

}  // namespace analyzer
