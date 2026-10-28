// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/symbols.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/parse.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

text::JsonValue make_symbol(std::string name,
                            SymbolKind kind,
                            text::JsonValue range,
                            text::JsonValue selection) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("name", text::JsonValue::make_string(std::move(name)));
  out.set("kind", text::JsonValue::make_number(static_cast<f64>(kind)));
  out.set("range", std::move(range));
  out.set("selectionRange", std::move(selection));
  return out;
}

text::JsonValue with_children(text::JsonValue out, text::JsonValue children) {
  if (!children.as_array().empty()) {
    out.set("children", std::move(children));
  }
  return out;
}

text::JsonValue range_of(const Document& document, diag::Span span) {
  return range_json(document, span.offset, span.length);
}

diag::Span type_span(const ast::AstArena& ast, ast::TypeIdx type) {
  if (type == ast::TypeIdx::invalid()) {
    return diag::Span{};
  }
  return ast.types[type].span;
}

std::string_view type_text(const ast::AstArena& ast,
                           std::string_view source,
                           ast::TypeIdx type) {
  if (type == ast::TypeIdx::invalid()) {
    return {};
  }
  const diag::Span span = ast.types[type].span;
  if (span.offset + span.length > source.size()) {
    return {};
  }
  return source.substr(span.offset, span.length);
}

// A field or variant has no single span of its own: its name and its
// type each have one, and the outline's range is what covers both.
diag::Span join(diag::Span name, diag::Span type) {
  if (type.length == 0) {
    return name;
  }
  const u32 start = name.offset < type.offset ? name.offset : type.offset;
  const u32 name_end = name.offset + name.length;
  const u32 type_end = type.offset + type.length;
  const u32 end = name_end > type_end ? name_end : type_end;
  return diag::Span{name.file, start, end - start};
}

text::JsonValue item_symbol(const Document& document,
                            const ast::AstArena& ast,
                            ast::ItemIdx index) {
  const ast::ItemNode& item = ast.items[index];
  using K = ast::ItemKind;
  switch (item.kind) {
    case K::Fn: {
      const ast::ItemFn& fn = item.payload.get<ast::ItemFn>();
      return make_symbol(std::string(fn.name.name), SymbolKind::Function,
                         range_of(document, item.span),
                         range_of(document, fn.name.span));
    }
    case K::Intrinsic: {
      const ast::ItemIntrinsic& fn = item.payload.get<ast::ItemIntrinsic>();
      return make_symbol(std::string(fn.name.name), SymbolKind::Function,
                         range_of(document, item.span),
                         range_of(document, fn.name.span));
    }
    case K::Extern: {
      text::JsonValue children = text::JsonValue::make_array();
      for (const ast::ItemExternFn& fn :
           item.payload.get<ast::ItemExtern>().fns) {
        children.push(make_symbol(std::string(fn.name.name),
                                  SymbolKind::Function,
                                  range_of(document, fn.name.span),
                                  range_of(document, fn.name.span)));
      }
      return with_children(make_symbol("<extern>", SymbolKind::Object,
                                       range_of(document, item.span),
                                       range_of(document, item.span)),
                           std::move(children));
    }
    case K::Struct: {
      const ast::ItemStruct& declaration = item.payload.get<ast::ItemStruct>();
      text::JsonValue children = text::JsonValue::make_array();
      for (const ast::ItemStructField& field : declaration.fields) {
        children.push(make_symbol(
            std::string(field.name.name), SymbolKind::Field,
            range_of(document,
                     join(field.name.span, type_span(ast, field.type))),
            range_of(document, field.name.span)));
      }
      return with_children(
          make_symbol(std::string(declaration.name.name), SymbolKind::Struct,
                      range_of(document, item.span),
                      range_of(document, declaration.name.span)),
          std::move(children));
    }
    case K::Enum: {
      const ast::ItemEnum& declaration = item.payload.get<ast::ItemEnum>();
      text::JsonValue children = text::JsonValue::make_array();
      for (const ast::ItemEnumVariant& variant : declaration.variants) {
        children.push(make_symbol(std::string(variant.name.name),
                                  SymbolKind::EnumMember,
                                  range_of(document, variant.name.span),
                                  range_of(document, variant.name.span)));
      }
      return with_children(
          make_symbol(std::string(declaration.name.name), SymbolKind::Enum,
                      range_of(document, item.span),
                      range_of(document, declaration.name.span)),
          std::move(children));
    }
    case K::Impl: {
      const ast::ItemImpl& declaration = item.payload.get<ast::ItemImpl>();
      std::string name =
          std::string(type_text(ast, document.text(), declaration.type));
      if (declaration.spec != ast::TypeIdx::invalid()) {
        name = std::string(type_text(ast, document.text(), declaration.spec)) +
               " for " + name;
      }
      text::JsonValue children = text::JsonValue::make_array();
      for (const ast::ItemIdx method : declaration.methods) {
        children.push(item_symbol(document, ast, method));
      }
      return with_children(
          make_symbol(std::move(name), SymbolKind::Object,
                      range_of(document, item.span),
                      range_of(document, type_span(ast, declaration.type))),
          std::move(children));
    }
    case K::Spec: {
      const ast::ItemSpec& declaration = item.payload.get<ast::ItemSpec>();
      text::JsonValue children = text::JsonValue::make_array();
      for (const ast::SpecMethod& method : declaration.methods) {
        children.push(make_symbol(std::string(method.name.name),
                                  SymbolKind::Method,
                                  range_of(document, method.name.span),
                                  range_of(document, method.name.span)));
      }
      return with_children(
          make_symbol(std::string(declaration.name.name), SymbolKind::Interface,
                      range_of(document, item.span),
                      range_of(document, declaration.name.span)),
          std::move(children));
    }
    case K::Static: {
      const ast::ItemStatic& declaration = item.payload.get<ast::ItemStatic>();
      return make_symbol(std::string(declaration.name.name),
                         SymbolKind::Variable, range_of(document, item.span),
                         range_of(document, declaration.name.span));
    }
    case K::Const: {
      const ast::ItemConst& declaration = item.payload.get<ast::ItemConst>();
      return make_symbol(std::string(declaration.name.name),
                         SymbolKind::Constant, range_of(document, item.span),
                         range_of(document, declaration.name.span));
    }
    case K::Use: {
      const ast::ItemUse& declaration = item.payload.get<ast::ItemUse>();
      std::string name;
      diag::Span selection = item.span;
      if (declaration.has_alias) {
        name = std::string(declaration.alias.name);
        selection = declaration.alias.span;
      } else if (declaration.path != ast::PathIdx::invalid()) {
        const ast::Path& path = ast.paths[declaration.path];
        if (!path.segments.empty()) {
          name = std::string(path.segments.back().name);
          selection = path.segments.back().span;
        }
      }
      return make_symbol(std::move(name), SymbolKind::Module,
                         range_of(document, item.span),
                         range_of(document, selection));
    }
  }
  return text::JsonValue{};
}

}  // namespace

text::JsonValue document_symbols(const Document& document) {
  pipeline::PipelineContext context{i18n::Language::EnUs};
  const source::FileId root =
      context.sources.add_virtual(document.uri(), document.text());
  const std::array<source::FileId, 1> files{root};
  base::Result<pipeline::ParsedFiles, diag::Reported> parsed =
      pipeline::parse_files(context, files);
  text::JsonValue symbols = text::JsonValue::make_array();
  if (parsed.is_err()) {
    return symbols;
  }
  const pipeline::ParsedFile* file = parsed.value().find(root);
  if (file == nullptr) {
    return symbols;
  }
  for (const ast::ItemIdx index : file->items) {
    symbols.push(item_symbol(document, context.ast, index));
  }
  return symbols;
}

}  // namespace lsp
