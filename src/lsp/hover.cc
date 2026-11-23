// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/hover.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_display.h"
#include "lsp/analysis.h"
#include "lsp/ast_query.h"
#include "lsp/document.h"
#include "lsp/layout.h"
#include "lsp/navigation.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace lsp {

namespace {

std::string signature_markdown(const pipeline::PipelineContext& context,
                               const ir::StorageState& state,
                               std::string_view keyword,
                               std::string_view name,
                               std::span<const ir::TypeIdx> params,
                               ir::TypeIdx ret,
                               std::string_view variadic_name) {
  std::string out =
      "~~~alcy\n" + std::string(keyword) + " " + std::string(name) + "(";
  bool first = true;
  for (const ir::TypeIdx param : params) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += ir::type_display(state, context.strings, param);
  }
  if (!variadic_name.empty()) {
    if (!first) {
      out += ", ";
    }
    out += variadic_name;
    out += ": ..";
  }
  out += ')';
  if (ret != ir::primitive_idx(ir::TypeTag::Void)) {
    out += " -> " + ir::type_display(state, context.strings, ret);
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

std::string_view span_text(std::string_view source, diag::Span span) {
  if (span.offset + span.length > source.size()) {
    return {};
  }
  return source.substr(span.offset, span.length);
}

// The doc comments the parser attached to an item, as markdown. Each span
// covers the whole `///` line; the marker and one following space are
// presentation, not part of the text the author wrote.
std::string doc_markdown(std::string_view source,
                         std::span<const diag::Span> docs) {
  if (docs.empty()) {
    return {};
  }
  std::string out = "\n";
  bool first = true;
  for (const diag::Span span : docs) {
    std::string_view line = span_text(source, span);
    if (line.starts_with("///")) {
      line.remove_prefix(3);
    }
    if (line.starts_with(' ')) {
      line.remove_prefix(1);
    }
    if (!first) {
      out.push_back('\n');
    }
    out.append(line);
    first = false;
  }
  return out;
}

// One line naming a type's size and alignment, or nothing for a type
// that has no layout until an argument is known.
std::string layout_line(const pipeline::PipelineContext& context,
                        const ir::StorageState& state,
                        ir::TypeIdx type) {
  const ir::TypeNode& node = state.types[type];
  if (node.tag == ir::TypeTag::Param || node.tag == ir::TypeTag::Const ||
      node.tag == ir::TypeTag::Error) {
    return {};
  }
  const ir::TypeLayout layout =
      ir::type_layout(state, type, context.target.width);
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
                                    const ir::StorageState& state,
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
                   ir::type_display(state, context.strings, named.type) +
                   "\n~~~\n" + layout_line(context, state, named.type);
          }
        }
      }
    }
  }
  return "~~~alcy\n" + std::string(written) + "\n~~~\n";
}

// The type of an expression, as the hover markdown shared by a use site
// and a declaration's initializer.
std::string type_markdown(const pipeline::PipelineContext& context,
                          const ir::StorageState& state,
                          ir::TypeIdx type) {
  return "~~~alcy\n" + ir::type_display(state, context.strings, type) +
         "\n~~~\n" + layout_line(context, state, type);
}

constexpr u32 NO_MEMBER = U32_MAX;

struct HoverTarget {
  ast::ItemIdx item = ast::ItemIdx::invalid();
  u32 member = NO_MEMBER;
};

// The declaration a recorded span names, when it belongs to an item.
std::optional<HoverTarget> target_of(const pipeline::PipelineContext& context,
                                     const analyzer::CheckedPackage& package,
                                     diag::Span declaration) {
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const ast::ItemIdx index : module->items) {
      const ast::ItemNode& item = context.ast.items[index];
      if (same_span(::ast::item_name_span(context.ast, item), declaration)) {
        return HoverTarget{index, NO_MEMBER};
      }
      using K = ast::ItemKind;
      if (item.kind == K::Struct) {
        const auto& fields = item.payload.get<ast::ItemStruct>().fields;
        for (u32 i = 0; i < fields.size(); ++i) {
          if (same_span(fields[i].name.span, declaration)) {
            return HoverTarget{index, i};
          }
        }
      } else if (item.kind == K::Enum) {
        const auto& variants = item.payload.get<ast::ItemEnum>().variants;
        for (u32 i = 0; i < variants.size(); ++i) {
          if (same_span(variants[i].name.span, declaration)) {
            return HoverTarget{index, i};
          }
        }
      } else if (item.kind == K::Impl) {
        for (const ast::ItemIdx method :
             item.payload.get<ast::ItemImpl>().methods) {
          if (same_span(
                  ::ast::item_name_span(context.ast, context.ast.items[method]),
                  declaration)) {
            return HoverTarget{method, NO_MEMBER};
          }
        }
      }
    }
  }
  return std::nullopt;
}

const analyzer::CheckedModule::StaticInfo* find_static_of(
    const analyzer::CheckedPackage& package,
    ast::ItemIdx item) {
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      if (info.item == item) {
        return &info;
      }
    }
  }
  return nullptr;
}

std::optional<std::string> render_target(const DocumentAnalysis& analysis,
                                         const HoverTarget& target,
                                         std::string_view source) {
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const ir::StorageState& state = analysis.types();
  const ast::ItemNode& item = context.ast.items[target.item];
  using K = ast::ItemKind;
  switch (item.kind) {
    case K::Struct: {
      const std::optional<StructLayout> layout =
          struct_layout_of(analysis, source, target.item);
      if (!layout.has_value()) {
        return std::nullopt;
      }
      if (target.member == NO_MEMBER) {
        return struct_layout_markdown(*layout) +
               doc_markdown(source, item.docs);
      }
      if (target.member >= layout->fields.size()) {
        return std::nullopt;
      }
      const FieldLayout& field = layout->fields[target.member];
      return "~~~alcy\n" + layout->name + "." + field.name + ": " + field.type +
             "\n~~~\n\nsize " + std::to_string(field.size) + " bytes, align " +
             std::to_string(field.align) + ", offset " +
             std::to_string(field.offset) + "\n";
    }
    case K::Enum: {
      const std::optional<EnumLayout> layout =
          enum_layout_of(analysis, source, target.item);
      if (!layout.has_value()) {
        return std::nullopt;
      }
      if (target.member == NO_MEMBER) {
        return enum_layout_markdown(*layout) + doc_markdown(source, item.docs);
      }
      if (target.member >= layout->variants.size()) {
        return std::nullopt;
      }
      const EnumVariantLayout& variant = layout->variants[target.member];
      std::string out = "~~~alcy\n" + layout->name + "::" + variant.name;
      if (!variant.fields.empty()) {
        out += '(';
        for (usize j = 0; j < variant.fields.size(); ++j) {
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
    case K::Fn:
    case K::Intrinsic: {
      const analyzer::CheckedModule::FnSig* signature =
          find_signature(package, target.item);
      if (signature == nullptr) {
        return std::nullopt;
      }
      return signature_markdown(context, state,
                                item.kind == K::Fn ? "fn" : "intrinsic",
                                item.name(), signature->params, signature->ret,
                                variadic_param_name(context, signature->item)) +
             doc_markdown(source, item.docs);
    }
    case K::Static:
    case K::Const: {
      const analyzer::CheckedModule::StaticInfo* info =
          find_static_of(package, target.item);
      if (info == nullptr) {
        return std::nullopt;
      }
      return "~~~alcy\n" +
             std::string(item.kind == K::Const ? "const " : "static ") +
             std::string(item.name()) + ": " +
             ir::type_display(state, context.strings, info->type) + "\n~~~\n" +
             doc_markdown(source, item.docs);
    }
    default: return std::nullopt;
  }
}

}  // namespace

std::optional<std::string> hover_markdown(const Document& document,
                                          const DocumentAnalysis& analysis,
                                          Position position) {
  if (!analysis.ok()) {
    return std::nullopt;
  }
  const usize offset = document.index().byte_offset(document.text(), position);
  const pipeline::PipelineContext& context = analysis.context();
  const analyzer::CheckedPackage& package = analysis.package();
  const ir::StorageState& state = analysis.types();
  const source::FileId root = analysis.root();
  const std::string_view source = document.text();

  if (const std::optional<BindingType> binding =
          binding_type_at(context, offset, root);
      binding.has_value()) {
    if (binding->init != ast::ExprIdx::invalid()) {
      if (const std::optional<ir::TypeIdx> type =
              checked_expr_type(package, binding->init);
          type.has_value()) {
        return type_markdown(context, state, *type);
      }
    }
    if (binding->written.length != 0 &&
        binding->written.offset + binding->written.length <= source.size()) {
      return "~~~alcy\n" +
             std::string(source.substr(binding->written.offset,
                                       binding->written.length)) +
             "\n~~~\n";
    }
  }
  const std::optional<diag::Span> declaration =
      definition_at(document, analysis, position);
  if (declaration.has_value()) {
    const std::optional<HoverTarget> target =
        target_of(context, package, *declaration);
    if (target.has_value()) {
      const std::optional<std::string_view> declaration_source =
          analysis.context().sources.bytes(declaration->file);
      if (declaration_source.has_value()) {
        return render_target(analysis, *target, *declaration_source);
      }
    }
  }
  if (const std::optional<ast::TypeIdx> type =
          innermost_type(context, offset, root);
      type.has_value()) {
    return type_reference_markdown(context, package, state, source, *type);
  }
  if (const std::optional<ast::ExprIdx> expr =
          innermost_expr(context, offset, root);
      expr.has_value()) {
    const std::optional<ir::TypeIdx> type = checked_expr_type(package, *expr);
    if (type.has_value()) {
      return type_markdown(context, state, *type);
    }
  }
  return std::nullopt;
}

}  // namespace lsp
