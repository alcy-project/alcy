// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/inlay_hints.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/layout.h"
#include "lsp/protocol.h"
#include "lsp/typing.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

// LSP InlayHintKind: the only kind alcy emits is Type. A layout hint is
// alcy-specific and carries no standard kind.
constexpr f64 K_TYPE = 1;

struct Hint {
  usize offset = 0;
  std::string label;
  bool is_type = false;
};

// A binding that wrote no type shows the type the initializer decided.
void collect_binding_hints(const pipeline::PipelineContext& context,
                           const analyzer::CheckedPackage& package,
                           source::FileId root,
                           std::vector<Hint>* out) {
  context.ast.stmts.for_each_node([&](const ast::StmtIdx index) {
    const ast::StmtNode& statement = context.ast.stmts[index];
    if (statement.span.file != root || statement.kind != ast::StmtKind::Decl) {
      return;
    }
    const ast::StmtDecl& declaration = statement.payload.get<ast::StmtDecl>();
    if (declaration.type != ast::TypeIdx::invalid() ||
        declaration.init == ast::ExprIdx::invalid()) {
      return;
    }
    const ast::PatternNode& pattern = context.ast.patterns[declaration.pattern];
    diag::Span name;
    if (pattern.kind == ast::PatternKind::Ident) {
      name = pattern.payload.ident.name.span;
    } else if (pattern.kind == ast::PatternKind::MutIdent) {
      name = pattern.payload.mut_ident.name.span;
    } else {
      return;
    }
    const std::optional<ir::TypeIdx> type =
        checked_expr_type(package, declaration.init);
    if (!type.has_value()) {
      return;
    }
    out->push_back(
        Hint{name.offset + name.length,
             ": " + type_name(package.types->state(), context.strings, *type),
             true});
  });
}

void collect_layout_hints(const pipeline::PipelineContext& context,
                          const analyzer::CheckedPackage& package,
                          std::string_view source,
                          source::FileId root,
                          std::vector<Hint>* out) {
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const ast::ItemIdx index : module->items) {
      const ast::ItemNode& item = context.ast.items[index];
      if (item.span.file != root) {
        continue;
      }
      if (item.kind == ast::ItemKind::Struct) {
        const std::optional<StructLayout> layout =
            struct_layout_of(context, package, source, index);
        if (!layout.has_value() || layout->generic) {
          continue;
        }
        const ast::ItemStruct& declaration =
            item.payload.get<ast::ItemStruct>();
        out->push_back(
            Hint{declaration.name.span.offset + declaration.name.span.length,
                 "size " + std::to_string(layout->size) + ", align " +
                     std::to_string(layout->align),
                 false});
        for (u32 i = 0;
             i < layout->fields.size() && i < declaration.fields.size(); ++i) {
          const ast::TypeIdx field_type = declaration.fields[i].type;
          if (field_type == ast::TypeIdx::invalid()) {
            continue;
          }
          const diag::Span span = context.ast.types[field_type].span;
          out->push_back(Hint{
              span.offset + span.length,
              "offset " + std::to_string(layout->fields[i].offset), false});
        }
      } else if (item.kind == ast::ItemKind::Enum) {
        const std::optional<EnumLayout> layout =
            enum_layout_of(context, package, source, index);
        if (!layout.has_value() || layout->generic) {
          continue;
        }
        const ast::ItemEnum& declaration = item.payload.get<ast::ItemEnum>();
        out->push_back(
            Hint{declaration.name.span.offset + declaration.name.span.length,
                 "size " + std::to_string(layout->size) + ", align " +
                     std::to_string(layout->align),
                 false});
      }
    }
  }
}

}  // namespace

text::JsonValue inlay_hints(const Document& document) {
  pipeline::PipelineContext context{i18n::Language::EnUs};
  source::FileId root = source::UNKNOWN_FILE;
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyze_document(context, document, &root);
  if (checked.is_err()) {
    return text::JsonValue::make_array();
  }
  const analyzer::CheckedPackage& package = checked.value();

  std::vector<Hint> hints;
  collect_binding_hints(context, package, root, &hints);
  collect_layout_hints(context, package, document.text(), root, &hints);
  std::sort(hints.begin(), hints.end(),
            [](const Hint& left, const Hint& right) {
              if (left.offset != right.offset) {
                return left.offset < right.offset;
              }
              if (left.is_type != right.is_type) {
                return left.is_type;
              }
              return left.label < right.label;
            });

  text::JsonValue array = text::JsonValue::make_array();
  for (const Hint& hint : hints) {
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("position", position_json(document.index().position_at(
                              document.text(), hint.offset)));
    entry.set("label", text::JsonValue::make_string(hint.label));
    if (hint.is_type) {
      entry.set("kind", text::JsonValue::make_number(K_TYPE));
    }
    entry.set("paddingLeft", text::JsonValue::make_bool(true));
    entry.set("paddingRight", text::JsonValue::make_bool(true));
    array.push(std::move(entry));
  }
  return array;
}

}  // namespace lsp
