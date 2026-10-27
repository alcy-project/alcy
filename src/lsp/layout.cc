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
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/typing.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

bool span_contains(diag::Span span, usize offset) {
  return span.offset <= offset && offset < span.offset + span.length;
}

// The source text a type annotation was written as. A generic
// declaration has no interned type to name, so its fields fall back to
// what the declaration spelled.
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

// The struct info that describes a declaration: the non-generic
// instantiation when the checker made one, otherwise the first entry the
// declaration has.
const analyzer::CheckedModule::StructInfo* choose_struct(
    const analyzer::CheckedPackage& package,
    ast::ItemIdx item) {
  const ir::StorageState& state = package.types->state();
  const analyzer::CheckedModule::StructInfo* fallback = nullptr;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
      if (info.item != item) {
        continue;
      }
      const ir::TypeNode& type = state.types[info.type];
      if (type.tag != ir::TypeTag::Struct) {
        continue;
      }
      if (state.struct_types[type.as_struct()].params.empty()) {
        return &info;
      }
      if (fallback == nullptr) {
        fallback = &info;
      }
    }
  }
  return fallback;
}

const analyzer::CheckedModule::EnumInfo* choose_enum(
    const analyzer::CheckedPackage& package,
    ast::ItemIdx item) {
  const ir::StorageState& state = package.types->state();
  const analyzer::CheckedModule::EnumInfo* fallback = nullptr;
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      if (info.item != item) {
        continue;
      }
      const ir::TypeNode& type = state.types[info.type];
      if (type.tag != ir::TypeTag::Enum) {
        continue;
      }
      if (state.enum_types[type.as_enum()].params.empty()) {
        return &info;
      }
      if (fallback == nullptr) {
        fallback = &info;
      }
    }
  }
  return fallback;
}

}  // namespace

std::optional<StructLayout> struct_layout_of(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    std::string_view source,
    ast::ItemIdx item_index) {
  const ast::ItemNode& item = context.ast.items[item_index];
  if (item.kind != ast::ItemKind::Struct) {
    return std::nullopt;
  }
  const ast::ItemStruct& declaration = item.payload.get<ast::ItemStruct>();
  const ir::StorageState& state = package.types->state();

  StructLayout layout;
  layout.name = std::string(declaration.name.name);

  const bool generic = !declaration.params.empty();
  const analyzer::CheckedModule::StructInfo* info =
      generic ? nullptr : choose_struct(package, item_index);
  if (generic || info == nullptr) {
    layout.generic = true;
    for (const ast::ItemStructField& field : declaration.fields) {
      layout.fields.push_back(FieldLayout{
          std::string(field.name.name),
          std::string(type_text(context.ast, source, field.type)),
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
    }
    entry.type = type_name(state, context.strings, field_type);
    entry.offset =
        ir::field_offset(state, struct_type.fields, i, context.target.width);
    entry.size = field.size;
    entry.align = field.align;
    layout.fields.push_back(std::move(entry));
  }
  return layout;
}

std::optional<EnumLayout> enum_layout_of(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    std::string_view source,
    ast::ItemIdx item_index) {
  const ast::ItemNode& item = context.ast.items[item_index];
  if (item.kind != ast::ItemKind::Enum) {
    return std::nullopt;
  }
  const ast::ItemEnum& declaration = item.payload.get<ast::ItemEnum>();
  const ir::StorageState& state = package.types->state();

  EnumLayout layout;
  layout.name = std::string(declaration.name.name);

  const bool generic = !declaration.params.empty();
  const analyzer::CheckedModule::EnumInfo* info =
      generic ? nullptr : choose_enum(package, item_index);
  if (generic || info == nullptr) {
    layout.generic = true;
    for (u32 i = 0; i < declaration.variants.size(); ++i) {
      const ast::ItemEnumVariant& variant = declaration.variants[i];
      EnumVariantLayout entry;
      entry.name = std::string(variant.name.name);
      entry.tag = i;
      for (u32 j = 0; j < variant.fields.size(); ++j) {
        entry.fields.push_back(FieldLayout{
            std::to_string(j),
            std::string(type_text(context.ast, source, variant.fields[j])),
            0,
            0,
            0,
        });
      }
      layout.variants.push_back(std::move(entry));
    }
    return layout;
  }

  const ir::EnumType& enum_type =
      state.enum_types[state.types[info->type].as_enum()];
  const ir::TypeLayout whole =
      ir::type_layout(state, info->type, context.target.width);
  const ir::TypeLayout area =
      ir::enum_payload_area(state, info->type, context.target.width);
  layout.size = whole.size;
  layout.align = whole.align;
  layout.payload_size = area.size;
  layout.payload_align = area.align;
  layout.payload_offset = ir::align_up(layout.discriminant_size, area.align);

  for (u32 i = 0; i < enum_type.variants.size(); ++i) {
    const ir::EnumVariantType& variant =
        state.enum_variant_types[enum_type.variants[i]];
    EnumVariantLayout entry;
    entry.name = std::string(context.strings.get(variant.name));
    entry.tag = i;
    entry.offset = layout.payload_offset;
    entry.size =
        ir::fields_layout(state, variant.fields, context.target.width).size;
    for (u32 j = 0; j < variant.fields.size(); ++j) {
      const ir::TypeIdx field_type = variant.fields[j];
      const ir::TypeLayout field =
          ir::type_layout(state, field_type, context.target.width);
      FieldLayout field_entry;
      field_entry.name = std::to_string(j);
      field_entry.type = type_name(state, context.strings, field_type);
      field_entry.offset =
          ir::field_offset(state, variant.fields, j, context.target.width);
      field_entry.size = field.size;
      field_entry.align = field.align;
      entry.fields.push_back(std::move(field_entry));
    }
    layout.variants.push_back(std::move(entry));
  }
  return layout;
}

std::optional<StructLayout> struct_layout_at(const Document& document,
                                             Position position) {
  const usize offset = document.index().byte_offset(document.text(), position);
  pipeline::PipelineContext context{i18n::Language::EnUs};
  source::FileId root = source::UNKNOWN_FILE;
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyze_document(context, document, &root);
  if (checked.is_err()) {
    return std::nullopt;
  }
  const analyzer::CheckedPackage& package = checked.value();
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const ast::ItemIdx index : module->items) {
      const ast::ItemNode& item = context.ast.items[index];
      if (item.kind == ast::ItemKind::Struct &&
          item.payload.get<ast::ItemStruct>().name.span.file == root &&
          span_contains(item.payload.get<ast::ItemStruct>().name.span,
                        offset)) {
        return struct_layout_of(context, package, document.text(), index);
      }
    }
  }
  return std::nullopt;
}

std::optional<EnumLayout> enum_layout_at(const Document& document,
                                         Position position) {
  const usize offset = document.index().byte_offset(document.text(), position);
  pipeline::PipelineContext context{i18n::Language::EnUs};
  source::FileId root = source::UNKNOWN_FILE;
  base::Result<analyzer::CheckedPackage, diag::Reported> checked =
      analyze_document(context, document, &root);
  if (checked.is_err()) {
    return std::nullopt;
  }
  const analyzer::CheckedPackage& package = checked.value();
  for (const analyzer::ModuleNode* module : package.tree.modules) {
    for (const ast::ItemIdx index : module->items) {
      const ast::ItemNode& item = context.ast.items[index];
      if (item.kind == ast::ItemKind::Enum &&
          item.payload.get<ast::ItemEnum>().name.span.file == root &&
          span_contains(item.payload.get<ast::ItemEnum>().name.span, offset)) {
        return enum_layout_of(context, package, document.text(), index);
      }
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

std::string enum_layout_markdown(const EnumLayout& layout) {
  std::string out;
  out += "~~~alcy\nenum " + layout.name + "\n~~~\n\n";
  if (layout.generic) {
    out +=
        "A generic declaration: the size and offsets depend on the "
        "type arguments.\n\n";
  } else {
    out += "size **" + std::to_string(layout.size) + "** bytes, align **" +
           std::to_string(layout.align) + "**; discriminant **" +
           std::to_string(layout.discriminant_size) + "** bytes, payload at " +
           std::to_string(layout.payload_offset) + " (size " +
           std::to_string(layout.payload_size) + ", align " +
           std::to_string(layout.payload_align) + ")\n\n";
  }
  out += "| tag | variant | field | offset | size | type |\n";
  out += "| ---: | --- | --- | ---: | ---: | --- |\n";
  const bool known = !layout.generic;
  for (const EnumVariantLayout& variant : layout.variants) {
    if (variant.fields.empty()) {
      out += "| " + std::to_string(variant.tag) + " | " + variant.name +
             " | - | - | - | - |\n";
      continue;
    }
    for (const FieldLayout& field : variant.fields) {
      out += "| " + std::to_string(variant.tag) + " | " + variant.name + " | " +
             field.name + " | " + cell(field.offset, known) + " | " +
             cell(field.size, known) + " | " + field.type + " |\n";
    }
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
