// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/type_hierarchy.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/ast_query.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

enum class TypeKind : u8 { Struct, Enum, Spec };

struct TypeRef {
  ast::ItemIdx item;
  TypeKind kind = TypeKind::Struct;
};

bool contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

bool same_span(diag::Span left, diag::Span right) {
  return left.file == right.file && left.offset == right.offset &&
         left.length == right.length;
}

f64 kind_number(TypeKind kind) {
  switch (kind) {
    case TypeKind::Struct: return static_cast<f64>(SymbolKind::Struct);
    case TypeKind::Enum: return static_cast<f64>(SymbolKind::Enum);
    case TypeKind::Spec: return static_cast<f64>(SymbolKind::Interface);
  }
  return static_cast<f64>(SymbolKind::Struct);
}

// The type or spec whose name span holds the offset.
std::optional<TypeRef> type_at(const pipeline::PipelineContext& context,
                               const analyzer::CheckedPackage& package,
                               usize offset,
                               source::FileId root) {
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const ast::ItemIdx index : module->items) {
      const ast::ItemNode& item = context.ast.items[index];
      const ast::ItemKind kind = item.kind;
      if (kind != ast::ItemKind::Struct && kind != ast::ItemKind::Enum &&
          kind != ast::ItemKind::Spec) {
        continue;
      }
      const diag::Span name = item_name_span(context.ast, item);
      if (name.file != root || !contains(name, offset)) {
        continue;
      }
      if (kind == ast::ItemKind::Struct) {
        return TypeRef{index, TypeKind::Struct};
      }
      if (kind == ast::ItemKind::Enum) {
        return TypeRef{index, TypeKind::Enum};
      }
      return TypeRef{index, TypeKind::Spec};
    }
  }
  return std::nullopt;
}

// The type or spec a name span declares.
std::optional<TypeRef> type_of_declaration(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    diag::Span name) {
  for (const analyzer::CheckedPackage::SpecDecl& spec : package.specs) {
    if (same_span(item_name_span(context.ast, context.ast.items[spec.item]),
                  name)) {
      return TypeRef{spec.item, TypeKind::Spec};
    }
  }
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
      if (info.item.is_valid() &&
          same_span(item_name_span(context.ast, context.ast.items[info.item]),
                    name)) {
        return TypeRef{info.item, TypeKind::Struct};
      }
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      if (info.item.is_valid() &&
          same_span(item_name_span(context.ast, context.ast.items[info.item]),
                    name)) {
        return TypeRef{info.item, TypeKind::Enum};
      }
    }
  }
  return std::nullopt;
}

// The name the target of an impl block spells, or nothing when the
// target is not a simple path.
std::string_view impl_target_name(const pipeline::PipelineContext& context,
                                  ast::ItemIdx impl_item) {
  const ast::ItemNode& item = context.ast.items[impl_item];
  if (item.kind != ast::ItemKind::Impl) {
    return {};
  }
  const ast::TypeIdx target = item.payload.get<ast::ItemImpl>().type;
  if (target == ast::TypeIdx::invalid()) {
    return {};
  }
  const ast::TypeNode& type = context.ast.types[target];
  if (type.kind != ast::TypeKind::Path) {
    return {};
  }
  const ast::PathIdx path = type.payload.get<ast::TypePath>().path;
  if (path == ast::PathIdx::invalid() ||
      context.ast.paths[path].segments.empty()) {
    return {};
  }
  return context.ast.paths[path].segments.back().name;
}

// The name a written type names, when it is a path.
std::string_view written_name(const pipeline::PipelineContext& context,
                              ast::TypeIdx type) {
  if (type == ast::TypeIdx::invalid()) {
    return {};
  }
  const ast::TypeNode& node = context.ast.types[type];
  if (node.kind != ast::TypeKind::Path) {
    return {};
  }
  const ast::PathIdx path = node.payload.get<ast::TypePath>().path;
  if (path == ast::PathIdx::invalid() ||
      context.ast.paths[path].segments.empty()) {
    return {};
  }
  return context.ast.paths[path].segments.back().name;
}

std::optional<u32> spec_index_of(const analyzer::CheckedPackage& package,
                                 ast::ItemIdx item) {
  for (u32 i = 0; i < package.specs.size(); ++i) {
    if (package.specs[i].item == item) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<TypeRef> find_spec(const pipeline::PipelineContext& context,
                                 const analyzer::CheckedPackage& package,
                                 std::string_view name) {
  for (const analyzer::CheckedPackage::SpecDecl& spec : package.specs) {
    if (spec.name == name) {
      return TypeRef{spec.item, TypeKind::Spec};
    }
  }
  (void)context;
  return std::nullopt;
}

std::optional<TypeRef> find_type(const pipeline::PipelineContext& context,
                                 const analyzer::CheckedPackage& package,
                                 source::FileId root,
                                 std::string_view name) {
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
      if (info.item.is_valid() &&
          item_name_span(context.ast, context.ast.items[info.item]).file ==
              root &&
          context.ast.items[info.item].name() == name) {
        return TypeRef{info.item, TypeKind::Struct};
      }
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      if (info.item.is_valid() &&
          item_name_span(context.ast, context.ast.items[info.item]).file ==
              root &&
          context.ast.items[info.item].name() == name) {
        return TypeRef{info.item, TypeKind::Enum};
      }
    }
  }
  return std::nullopt;
}

text::JsonValue item_json(const Document& document,
                          const pipeline::PipelineContext& context,
                          TypeRef ref) {
  const ast::ItemNode& node = context.ast.items[ref.item];
  const diag::Span name = item_name_span(context.ast, node);
  text::JsonValue item = text::JsonValue::make_object();
  item.set("name", text::JsonValue::make_string(std::string(node.name())));
  item.set("kind", text::JsonValue::make_number(kind_number(ref.kind)));
  item.set("uri", text::JsonValue::make_string(document.uri()));
  item.set("range", range_json(document, node.span.offset, node.span.length));
  item.set("selectionRange", range_json(document, name.offset, name.length));
  text::JsonValue data = text::JsonValue::make_object();
  data.set("nameOffset", text::JsonValue::make_number(name.offset));
  data.set("nameLength", text::JsonValue::make_number(name.length));
  item.set("data", std::move(data));
  return item;
}

const text::JsonValue* item_data(const text::JsonValue& item) {
  const text::JsonValue* data = item.find("data");
  if (data == nullptr || !data->is_object()) {
    return nullptr;
  }
  return data;
}

bool is_spec(const TypeRef& ref) {
  return ref.kind == TypeKind::Spec;
}

}  // namespace

text::JsonValue prepare_type_hierarchy(const Document& document,
                                       const DocumentAnalysis& analysis,
                                       Position position) {
  text::JsonValue items = text::JsonValue::make_array();
  if (!analysis.ok()) {
    return items;
  }
  const usize offset = document.index().byte_offset(document.text(), position);
  const std::optional<TypeRef> ref =
      type_at(analysis.context(), analysis.package(), offset, analysis.root());
  if (!ref.has_value()) {
    return items;
  }
  items.push(item_json(document, analysis.context(), *ref));
  return items;
}

text::JsonValue supertypes(const Document& document,
                           const DocumentAnalysis& analysis,
                           const text::JsonValue& item) {
  text::JsonValue out = text::JsonValue::make_array();
  const text::JsonValue* data = item_data(item);
  if (!analysis.ok() || data == nullptr) {
    return out;
  }
  const diag::Span name = [&] {
    diag::Span span;
    span.file = analysis.root();
    span.offset = static_cast<u32>(data->find("nameOffset")->as_number());
    span.length = static_cast<u32>(data->find("nameLength")->as_number());
    return span;
  }();
  const std::optional<TypeRef> ref =
      type_of_declaration(analysis.context(), analysis.package(), name);
  if (!ref.has_value()) {
    return out;
  }
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();

  if (is_spec(*ref)) {
    const ast::ItemSpec& declaration =
        context.ast.items[ref->item].payload.get<ast::ItemSpec>();
    const std::string_view super = written_name(context, declaration.super);
    if (!super.empty()) {
      const std::optional<TypeRef> found = find_spec(context, package, super);
      if (found.has_value()) {
        out.push(item_json(document, context, *found));
      }
    }
    return out;
  }

  const std::string_view target = context.ast.items[ref->item].name();
  for (const analyzer::CheckedPackage::SpecImplDecl& impl :
       package.spec_impls) {
    if (impl.item.is_valid() && impl.spec < package.specs.size() &&
        impl_target_name(context, impl.item) == target) {
      out.push(
          item_json(document, context,
                    TypeRef{package.specs[impl.spec].item, TypeKind::Spec}));
    }
  }
  return out;
}

text::JsonValue subtypes(const Document& document,
                         const DocumentAnalysis& analysis,
                         const text::JsonValue& item) {
  text::JsonValue out = text::JsonValue::make_array();
  const text::JsonValue* data = item_data(item);
  if (!analysis.ok() || data == nullptr) {
    return out;
  }
  const diag::Span name = [&] {
    diag::Span span;
    span.file = analysis.root();
    span.offset = static_cast<u32>(data->find("nameOffset")->as_number());
    span.length = static_cast<u32>(data->find("nameLength")->as_number());
    return span;
  }();
  const std::optional<TypeRef> ref =
      type_of_declaration(analysis.context(), analysis.package(), name);
  if (!ref.has_value() || !is_spec(*ref)) {
    return out;
  }
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const std::optional<u32> spec = spec_index_of(package, ref->item);
  if (!spec.has_value()) {
    return out;
  }
  for (const analyzer::CheckedPackage::SpecImplDecl& impl :
       package.spec_impls) {
    if (impl.spec != *spec || !impl.item.is_valid()) {
      continue;
    }
    const std::string_view target = impl_target_name(context, impl.item);
    if (target.empty()) {
      continue;
    }
    const std::optional<TypeRef> found =
        find_type(context, package, analysis.root(), target);
    if (found.has_value()) {
      out.push(item_json(document, context, *found));
    }
  }
  return out;
}

}  // namespace lsp
