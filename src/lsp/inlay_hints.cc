// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/inlay_hints.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "lsp/analysis.h"
#include "lsp/ast_query.h"
#include "lsp/document.h"
#include "lsp/layout.h"
#include "lsp/protocol.h"
#include "lsp/typing.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

struct Hint {
  usize offset = 0;
  std::string label;
  bool is_type = false;
  bool is_parameter = false;
};

// The name a parameter pattern binds, or nothing for a pattern that
// binds no single name.
std::string_view param_name(const ast::AstArena& ast,
                            const ast::ItemFnParam& param) {
  if (param.pattern == ast::PatternIdx::invalid()) {
    return {};
  }
  const ast::PatternNode& pattern = ast.patterns[param.pattern];
  if (pattern.kind == ast::PatternKind::Ident) {
    return pattern.payload.ident.name.name;
  }
  if (pattern.kind == ast::PatternKind::MutIdent) {
    return pattern.payload.mut_ident.name.name;
  }
  return {};
}

// A binding that wrote no type shows the type the initializer decided.
void collect_binding_hints(const DocumentAnalysis& analysis,
                           std::vector<Hint>* out) {
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();
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

void collect_layout_hints(const DocumentAnalysis& analysis,
                          std::string_view source,
                          std::vector<Hint>* out) {
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const ast::ItemIdx index : module->items) {
      const ast::ItemNode& item = context.ast.items[index];
      if (item.span.file != root) {
        continue;
      }
      if (item.kind == ast::ItemKind::Struct) {
        const std::optional<StructLayout> layout =
            struct_layout_of(analysis, source, index);
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
            enum_layout_of(analysis, source, index);
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

// A call whose callee the checker resolved shows the name of the
// parameter each argument binds, the way a reader names an argument at
// the call site.
void collect_parameter_hints(const DocumentAnalysis& analysis,
                             std::vector<Hint>* out) {
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();

  std::unordered_map<u64, ast::ItemIdx> item_by_declaration;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& sig : module.functions) {
      if (sig.item.is_valid()) {
        item_by_declaration.insert_or_assign(
            span_key(item_name_span(context.ast, context.ast.items[sig.item])),
            sig.item);
      }
    }
  }
  std::unordered_map<u64, u64> declaration_by_site;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.site.file == root && reference.declaration.file == root) {
        declaration_by_site[span_key(reference.site)] =
            span_key(reference.declaration);
      }
    }
  }

  context.ast.exprs.for_each_node([&](const ast::ExprIdx index) {
    const ast::ExprNode& node = context.ast.exprs[index];
    if (node.span.file != root) {
      return;
    }
    diag::Span site;
    std::span<const ast::ExprIdx> args;
    bool is_method = false;
    if (node.kind == ast::ExprKind::Call) {
      const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
      site = context.ast.exprs[call.callee].span;
      args = call.args;
    } else if (node.kind == ast::ExprKind::MethodCall) {
      const ast::ExprMethodCall& call = node.payload.get<ast::ExprMethodCall>();
      site = call.name.span;
      args = call.args;
      is_method = true;
    } else {
      return;
    }
    const auto declaration = declaration_by_site.find(span_key(site));
    if (declaration == declaration_by_site.end()) {
      return;
    }
    const auto item = item_by_declaration.find(declaration->second);
    if (item == item_by_declaration.end()) {
      return;
    }
    const ast::ItemNode& callee = context.ast.items[item->second];
    if (callee.kind != ast::ItemKind::Fn) {
      return;
    }
    const std::span<const ast::ItemFnParam> params =
        callee.payload.get<ast::ItemFn>().params;
    const usize offset = is_method && !params.empty() &&
                                 param_name(context.ast, params[0]) == "self"
                             ? 1
                             : 0;
    for (usize i = 0; i < args.size() && i + offset < params.size(); ++i) {
      const std::string_view name = param_name(context.ast, params[i + offset]);
      if (name.empty() || name == "self") {
        continue;
      }
      const ast::ExprNode& argument = context.ast.exprs[args[i]];
      if (argument.span.file != root || argument.span.length == 0) {
        continue;
      }
      if (argument.kind == ast::ExprKind::Path) {
        const ast::PathIdx path = argument.payload.get<ast::ExprPath>().idx;
        if (path != ast::PathIdx::invalid() &&
            !context.ast.paths[path].segments.empty() &&
            context.ast.paths[path].segments.back().name == name) {
          continue;
        }
      }
      out->push_back(
          Hint{argument.span.offset, std::string(name) + ":", false, true});
    }
  });
}

}  // namespace

text::JsonValue inlay_hints(const Document& document,
                            const DocumentAnalysis& analysis) {
  if (!analysis.ok()) {
    return text::JsonValue::make_array();
  }
  std::vector<Hint> hints;
  collect_binding_hints(analysis, &hints);
  collect_layout_hints(analysis, document.text(), &hints);
  collect_parameter_hints(analysis, &hints);
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
      entry.set("kind", text::JsonValue::make_number(
                            static_cast<f64>(InlayHintKind::Type)));
    } else if (hint.is_parameter) {
      entry.set("kind", text::JsonValue::make_number(
                            static_cast<f64>(InlayHintKind::Parameter)));
    }
    entry.set("paddingLeft", text::JsonValue::make_bool(!hint.is_parameter));
    entry.set("paddingRight", text::JsonValue::make_bool(true));
    array.push(std::move(entry));
  }
  return array;
}

}  // namespace lsp
