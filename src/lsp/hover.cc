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
#include "lsp/document.h"
#include "lsp/layout.h"
#include "lsp/typing.h"
#include "pipeline/pipeline_context.h"

namespace lsp {

namespace {

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

diag::Span name_span(const ast::AstArena& ast, const ast::ItemNode& item) {
  using K = ast::ItemKind;
  switch (item.kind) {
    case K::Struct: return item.payload.get<ast::ItemStruct>().name.span;
    case K::Enum: return item.payload.get<ast::ItemEnum>().name.span;
    case K::Fn: return item.payload.get<ast::ItemFn>().name.span;
    case K::Intrinsic: return item.payload.get<ast::ItemIntrinsic>().name.span;
    case K::Static: return item.payload.get<ast::ItemStatic>().name.span;
    case K::Const: return item.payload.get<ast::ItemConst>().name.span;
    case K::Spec: return item.payload.get<ast::ItemSpec>().name.span;
    case K::Impl: return ast.types[item.payload.get<ast::ItemImpl>().type].span;
    case K::Extern:
    case K::Use: return item.span;
  }
  return item.span;
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
          if (!span_contains(name_span(context.ast, item), offset)) {
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
          if (!span_contains(name_span(context.ast, item), offset)) {
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
  return std::nullopt;
}

}  // namespace lsp
