// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/layout.h"

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
#include "ir/storage.h"
#include "ir/type.h"
#include "lsp/document.h"
#include "pipeline/frontend.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

// The source text a type annotation was written as. The layout is the
// interned type's; the name a reader sees is the one the declaration
// spelled.
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

std::string cell(u64 value, bool known) {
  return known ? std::to_string(value) : std::string("-");
}

}  // namespace

std::optional<StructLayout> struct_layout_at(const Document& document,
                                             Position position) {
  const usize offset = document.index().byte_offset(document.text(), position);

  pipeline::PipelineContext context{i18n::Language::EnUs};
  const source::FileId root =
      context.sources.add_virtual(document.uri(), document.text());
  base::Result<analyzer::ModuleTree, diag::Reported> tree =
      pipeline::front_end_root(context, root, pipeline::full_std_selection());
  if (tree.is_err() || context.bag.has_errors()) {
    return std::nullopt;
  }
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      pipeline::analyze_tree(context, tree.value());
  if (checked.is_err()) {
    return std::nullopt;
  }
  const analyzer::CheckedPackage& package = checked.value();
  const ir::StorageState& state = package.types->state();

  for (u32 module_index = 0; module_index < package.tree.modules.size();
       ++module_index) {
    const analyzer::ModuleNode& module = *package.tree.modules[module_index];
    for (const ast::ItemIdx item_index : module.items) {
      const ast::ItemNode& item = context.ast.items[item_index];
      if (item.kind != ast::ItemKind::Struct) {
        continue;
      }
      const ast::ItemStruct& declaration = item.payload.get<ast::ItemStruct>();
      if (!span_contains(declaration.name.span, offset)) {
        continue;
      }

      StructLayout layout;
      layout.name = std::string(declaration.name.name);

      // The declaration's own parameters, when it has any, leave the
      // offsets open; only an instantiation has concrete numbers.
      const bool generic = !declaration.params.empty();
      const analyzer::CheckedModule::StructInfo* info = nullptr;
      if (!generic && module_index < package.modules.size()) {
        for (const analyzer::CheckedModule::StructInfo& candidate :
             package.modules[module_index].structs) {
          if (candidate.item != item_index) {
            continue;
          }
          const ir::TypeNode& type = state.types[candidate.type];
          if (type.tag != ir::TypeTag::Struct) {
            continue;
          }
          if (state.struct_types[type.as_struct()].params.empty()) {
            info = &candidate;
            break;
          }
          if (info == nullptr) {
            info = &candidate;
          }
        }
      }

      if (generic || info == nullptr) {
        layout.generic = true;
        for (const ast::ItemStructField& field : declaration.fields) {
          layout.fields.push_back(FieldLayout{
              std::string(field.name.name),
              std::string(type_text(context.ast, document.text(), field.type)),
              0,
              0,
              0,
          });
        }
        return layout;
      }

      const ir::StructType& struct_type =
          state.struct_types[state.types[info->type].as_struct()];
      const ir::TypeLayout whole =
          ir::type_layout(state, info->type, context.target.width);
      layout.size = whole.size;
      layout.align = whole.align;
      for (u32 i = 0; i < struct_type.fields.size(); ++i) {
        const ir::TypeIdx field_type = struct_type.fields[i];
        const ir::TypeLayout field =
            ir::type_layout(state, field_type, context.target.width);
        FieldLayout entry;
        if (i < declaration.fields.size()) {
          entry.name = std::string(declaration.fields[i].name.name);
          entry.type = std::string(type_text(context.ast, document.text(),
                                             declaration.fields[i].type));
        }
        entry.offset = ir::field_offset(state, struct_type.fields, i,
                                        context.target.width);
        entry.size = field.size;
        entry.align = field.align;
        layout.fields.push_back(std::move(entry));
      }
      return layout;
    }
  }
  return std::nullopt;
}

std::string struct_layout_markdown(const StructLayout& layout) {
  std::string out;
  out += "~~~alcy\nstruct " + layout.name + "\n~~~\n\n";
  if (layout.generic) {
    out +=
        "A generic declaration: the size and offsets depend on the "
        "type arguments.\n\n";
  } else {
    out += "size **" + std::to_string(layout.size) + "** bytes, align **" +
           std::to_string(layout.align) + "**\n\n";
  }
  out += "| offset | size | align | field | type |\n";
  out += "| ---: | ---: | ---: | --- | --- |\n";
  const bool known = !layout.generic;
  for (const FieldLayout& field : layout.fields) {
    out += "| " + cell(field.offset, known) + " | " + cell(field.size, known) +
           " | " + cell(field.align, known) + " | " + field.name + " | " +
           field.type + " |\n";
  }
  return out;
}

text::JsonValue struct_layout_json(const StructLayout& layout) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("name", text::JsonValue::make_string(layout.name));
  out.set("size", text::JsonValue::make_number(static_cast<f64>(layout.size)));
  out.set("align",
          text::JsonValue::make_number(static_cast<f64>(layout.align)));
  out.set("generic", text::JsonValue::make_bool(layout.generic));

  text::JsonValue fields = text::JsonValue::make_array();
  for (const FieldLayout& field : layout.fields) {
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("name", text::JsonValue::make_string(field.name));
    entry.set("type", text::JsonValue::make_string(field.type));
    entry.set("offset",
              text::JsonValue::make_number(static_cast<f64>(field.offset)));
    entry.set("size",
              text::JsonValue::make_number(static_cast<f64>(field.size)));
    entry.set("align",
              text::JsonValue::make_number(static_cast<f64>(field.align)));
    fields.push(std::move(entry));
  }
  out.set("fields", std::move(fields));
  return out;
}

}  // namespace lsp
