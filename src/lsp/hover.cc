// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/hover.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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
#include "ir/type.h"
#include "lsp/analysis.h"
#include "lsp/ast_query.h"
#include "lsp/document.h"
#include "lsp/layout.h"
#include "lsp/typing.h"
#include "pipeline/pipeline_context.h"

namespace lsp {

namespace {

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

std::string signature_markdown(const pipeline::PipelineContext& context,
                               const analyzer::CheckedPackage& package,
                               std::string_view keyword,
                               std::string_view name,
                               std::span<const ir::TypeIdx> params,
                               ir::TypeIdx ret) {
  const ir::StorageState& state = package.types->state();
  std::string out =
      "~~~alcy\n" + std::string(keyword) + " " + std::string(name) + "(";
  bool first = true;
  for (const ir::TypeIdx param : params) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += type_name(state, context.strings, param);
  }
  out += ')';
  if (ret != ir::primitive_idx(ir::TypeTag::Void)) {
    out += " -> " + type_name(state, context.strings, ret);
  }
  out += "\n~~~\n";
  return out;
}

const analyzer::CheckedModule::FnSig* find_signature(
    const analyzer::CheckedPackage& package,
    ast::ItemIdx item) {
  const analyzer::CheckedModule::FnSig* fallback = nullptr;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& sig : module.functions) {
      if (sig.item != item) {
        continue;
      }
      if (sig.inst == analyzer::NO_INST) {
        return &sig;
      }
      if (fallback == nullptr) {
        fallback = &sig;
      }
    }
  }
  return fallback;
}

const analyzer::CheckedModule::StaticInfo* find_static(
    const analyzer::CheckedPackage& package,
    std::string_view name) {
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      if (info.name == name) {
        return &info;
      }
    }
  }
  return nullptr;
}

std::string_view span_text(std::string_view source, diag::Span span) {
  if (span.offset + span.length > source.size()) {
    return {};
  }
  return source.substr(span.offset, span.length);
}

// The smallest type or expression whose span holds the cursor: the one
// the position is written inside, not the statement around it.
std::optional<ast::TypeIdx> innermost_type(
    const pipeline::PipelineContext& context,
    usize offset) {
  std::optional<ast::TypeIdx> best;
  usize best_length = 0;
  context.ast.types.for_each_node([&](const ast::TypeIdx index) {
    const diag::Span span = context.ast.types[index].span;
    if (!span_contains(span, offset)) {
      return;
    }
    if (!best.has_value() || span.length < best_length) {
      best = index;
      best_length = span.length;
    }
  });
  return best;
}

std::optional<ast::ExprIdx> innermost_expr(
    const pipeline::PipelineContext& context,
    usize offset) {
  std::optional<ast::ExprIdx> best;
  usize best_length = 0;
  context.ast.exprs.for_each_node([&](const ast::ExprIdx index) {
    const diag::Span span = context.ast.exprs[index].span;
    if (!span_contains(span, offset)) {
      return;
    }
    if (!best.has_value() || span.length < best_length) {
      best = index;
      best_length = span.length;
    }
  });
  return best;
}

// The type the checker decided for an expression. A checked entry exists
// once per instantiation; the declaration's own entry is the one a hover
// without a call site wants.
std::optional<ir::TypeIdx> expr_type(const analyzer::CheckedPackage& package,
                                     ast::ExprIdx expr) {
  std::optional<ir::TypeIdx> fallback;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::ExprType& entry : module.expr_types) {
      if (entry.expr != expr) {
        continue;
      }
      if (entry.inst == analyzer::NO_INST) {
        return entry.type;
      }
      if (!fallback.has_value()) {
        fallback = entry.type;
      }
    }
  }
  return fallback;
}

// One line naming a type's size and alignment, or nothing for a type
// that has no layout until an argument is known.
std::string layout_line(const pipeline::PipelineContext& context,
                        const analyzer::CheckedPackage& package,
                        ir::TypeIdx type) {
  const ir::TypeNode& node = package.types->state().types[type];
  if (node.tag == ir::TypeTag::Param || node.tag == ir::TypeTag::Const ||
      node.tag == ir::TypeTag::Error) {
    return {};
  }
  const ir::TypeLayout layout =
      ir::type_layout(package.types->state(), type, context.target.width);
  if (layout.size == 0) {
    return {};
  }
  return "\nsize " + std::to_string(layout.size) + " bytes, align " +
         std::to_string(layout.align) + "\n";
}

// A written type: a path that names a declaration shows the checked type
// and its layout, and every other spelling shows itself.
std::string type_reference_markdown(const pipeline::PipelineContext& context,
                                    const analyzer::CheckedPackage& package,
                                    std::string_view source,
                                    ast::TypeIdx index) {
  const ast::TypeNode& node = context.ast.types[index];
  const std::string_view written = span_text(source, node.span);
  if (node.kind == ast::TypeKind::Path) {
    const ast::TypePath& path = node.payload.get<ast::TypePath>();
    if (path.path != ast::PathIdx::invalid()) {
      const ast::Path& parsed = context.ast.paths[path.path];
      if (!parsed.segments.empty()) {
        const std::string_view name = parsed.segments.back().name;
        for (const analyzer::CheckedModule& module : package.modules) {
          for (const analyzer::CheckedModule::NamedType& named : module.types) {
            if (named.name != name) {
              continue;
            }
            return "~~~alcy\n" +
                   type_name(package.types->state(), context.strings,
                             named.type) +
                   "\n~~~\n" + layout_line(context, package, named.type);
          }
        }
      }
    }
  }
  return "~~~alcy\n" + std::string(written) + "\n~~~\n";
}

}  // namespace

std::optional<std::string> hover_markdown(const Document& document,
                                          Position position) {
  const usize offset = document.index().byte_offset(document.text(), position);
  pipeline::PipelineContext context{i18n::Language::EnUs};
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyze_document(context, document);
  if (checked.is_err()) {
    return std::nullopt;
  }
  const analyzer::CheckedPackage& package = checked.value();
  const std::string_view source = document.text();

  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const ast::ItemIdx index : module->items) {
      const ast::ItemNode& item = context.ast.items[index];
      using K = ast::ItemKind;
      switch (item.kind) {
        case K::Struct: {
          const ast::ItemStruct& declaration =
              item.payload.get<ast::ItemStruct>();
          if (span_contains(declaration.name.span, offset)) {
            const std::optional<StructLayout> layout =
                struct_layout_of(context, package, source, index);
            if (!layout.has_value()) {
              return std::nullopt;
            }
            return struct_layout_markdown(*layout);
          }
          for (u32 i = 0; i < declaration.fields.size(); ++i) {
            if (!span_contains(declaration.fields[i].name.span, offset)) {
              continue;
            }
            const std::optional<StructLayout> layout =
                struct_layout_of(context, package, source, index);
            if (!layout.has_value() || i >= layout->fields.size()) {
              return std::nullopt;
            }
            const FieldLayout& field = layout->fields[i];
            return "~~~alcy\n" + layout->name + "." + field.name + ": " +
                   field.type + "\n~~~\n\nsize " + std::to_string(field.size) +
                   " bytes, align " + std::to_string(field.align) +
                   ", offset " + std::to_string(field.offset) + "\n";
          }
          break;
        }
        case K::Enum: {
          const ast::ItemEnum& declaration = item.payload.get<ast::ItemEnum>();
          if (span_contains(declaration.name.span, offset)) {
            const std::optional<EnumLayout> layout =
                enum_layout_of(context, package, source, index);
            if (!layout.has_value()) {
              return std::nullopt;
            }
            return enum_layout_markdown(*layout);
          }
          for (u32 i = 0; i < declaration.variants.size(); ++i) {
            if (!span_contains(declaration.variants[i].name.span, offset)) {
              continue;
            }
            const std::optional<EnumLayout> layout =
                enum_layout_of(context, package, source, index);
            if (!layout.has_value() || i >= layout->variants.size()) {
              return std::nullopt;
            }
            const EnumVariantLayout& variant = layout->variants[i];
            std::string out = "~~~alcy\n" + layout->name + "::" + variant.name;
            if (!variant.fields.empty()) {
              out += '(';
              for (u32 j = 0; j < variant.fields.size(); ++j) {
                if (j != 0) {
                  out += ", ";
                }
                out += variant.fields[j].type;
              }
              out += ')';
            }
            out += "\n~~~\n";
            return out;
          }
          break;
        }
        case K::Fn:
        case K::Intrinsic: {
          const std::string_view name = item.name();
          if (!span_contains(item_name_span(context.ast, item), offset)) {
            break;
          }
          const analyzer::CheckedModule::FnSig* signature =
              find_signature(package, index);
          if (signature == nullptr) {
            return std::nullopt;
          }
          return signature_markdown(context, package,
                                    item.kind == K::Fn ? "fn" : "intrinsic",
                                    name, signature->params, signature->ret);
        }
        case K::Static:
        case K::Const: {
          const std::string_view name = item.name();
          if (!span_contains(item_name_span(context.ast, item), offset)) {
            break;
          }
          const analyzer::CheckedModule::StaticInfo* info =
              find_static(package, name);
          if (info == nullptr) {
            return std::nullopt;
          }
          const ir::StorageState& state = package.types->state();
          return "~~~alcy\n" +
                 std::string(item.kind == K::Const ? "const " : "static ") +
                 std::string(name) + ": " +
                 type_name(state, context.strings, info->type) + "\n~~~\n";
        }
        default: break;
      }
    }
  }
  if (const std::optional<ast::TypeIdx> type = innermost_type(context, offset);
      type.has_value()) {
    return type_reference_markdown(context, package, source, *type);
  }
  if (const std::optional<ast::ExprIdx> expr = innermost_expr(context, offset);
      expr.has_value()) {
    const std::optional<ir::TypeIdx> type = expr_type(package, *expr);
    if (type.has_value()) {
      return "~~~alcy\n" +
             type_name(package.types->state(), context.strings, *type) +
             "\n~~~\n" + layout_line(context, package, *type);
    }
  }
  return std::nullopt;
}

}  // namespace lsp
