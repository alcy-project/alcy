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

// Spells one type of the summary's tables into the syntax arena; invalid
// when the syntax cannot carry it. A nominal is left out until a summary
// carries its fields, and a raw pointer or constant has no spelling a
// dependent can use.
ast::TypeIdx spell_type(const ir::Storage& storage,
                        ast::AstArena& ast,
                        ir::TypeIdx type) {
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
      const ast::TypeIdx inner = spell_type(storage, ast, shape.pointee);
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
      const ast::TypeIdx element = spell_type(storage, ast, shape.element);
      if (!element.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      made.kind = ast::TypeKind::Slice;
      made.payload.set(ast::TypeSlice{.element = element});
      return ast.types.push_back(made);
    }
    case ir::TypeTag::Array: {
      const ir::ArrayType& shape = storage.array_types()[node.as_array()];
      const ast::TypeIdx element = spell_type(storage, ast, shape.element);
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
        const ast::TypeIdx spelled = spell_type(storage, ast, element);
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
        const ast::TypeIdx spelled = spell_type(storage, ast, param);
        if (!spelled.is_valid()) {
          return ast::TypeIdx::invalid();
        }
        params.push_back(spelled);
      }
      const ast::TypeIdx ret = spell_type(storage, ast, shape.ret);
      if (!ret.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      made.kind = ast::TypeKind::Func;
      made.payload.set(ast::TypeFunc{
          .params = ast::copy_to_arena(ast.spans, params), .ret = ret});
      return ast.types.push_back(made);
    }
    default: return ast::TypeIdx::invalid();
  }
}

// Spells one free function of the summary into an item: a wildcard per
// parameter, because a summary carries no pattern names, and no body,
// because the package's own artifact holds the code.
bool spell_function(const ir::Storage& storage,
                    ast::AstArena& ast,
                    std::string_view name,
                    ir::FunctionIdx function_index,
                    ast::ItemIdx& item_out) {
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
    const ast::TypeIdx spelled = spell_type(storage, ast, param);
    if (!spelled.is_valid()) {
      return false;
    }
    ast::PatternNode pattern;
    pattern.kind = ast::PatternKind::Wildcard;
    const ast::PatternIdx pattern_index = ast.patterns.push_back(pattern);
    params.push_back(
        ast::ItemFnParam{.pattern = pattern_index, .type = spelled});
  }
  const ast::TypeIdx ret = spell_type(storage, ast, meta.return_type);
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
  std::unordered_map<std::string, std::vector<ast::ItemIdx>> items_by_module;
  std::vector<std::string> order;
  for (const ir::SummaryEntry& row : summary.summary) {
    // What a dependent cannot use yet - a nominal, an associated
    // function, a method, or a declaration that needs its body - is left
    // for the slices the summary carries them in.
    if (row.kind != ir::SummaryKind::Free || row.needs_body ||
        !row.function.is_valid()) {
      continue;
    }
    const std::string_view name = named(row.name, "");
    const std::string_view path = named(row.path, "");
    ast::ItemIdx item = ast::ItemIdx::invalid();
    if (!spell_function(storage, ast, name, row.function, item)) {
      continue;
    }
    std::vector<ast::ItemIdx>& items = items_by_module[std::string(path)];
    if (items.empty()) {
      order.emplace_back(path);
    }
    items.push_back(item);
    if (ast.exhausted()) {
      return surface;
    }
  }
  // The items and the paths they belong to are copied into the arena, so
  // nothing here outlives the call.
  for (const std::string& path : order) {
    const std::vector<ast::ItemIdx>& items = items_by_module[path];
    surface.modules.push_back(
        SummarySurface::Module{path, ast::copy_to_arena(ast.spans, items)});
  }
  return surface;
}

}  // namespace analyzer
