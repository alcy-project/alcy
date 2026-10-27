// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "pipeline/pipeline_context.h"
#include "text/json.h"

namespace lsp {

// One field of a type's memory layout: where it starts, and the size and
// alignment its own type gives it.
struct FieldLayout {
  std::string name;
  std::string type;
  u64 offset = 0;
  u64 size = 0;
  u64 align = 1;
};

// The memory layout of one struct declaration. A generic declaration has
// no layout of its own, because its offsets depend on the instantiation,
// so generic marks that only the field names and written types are known.
struct StructLayout {
  std::string name;
  u64 size = 0;
  u64 align = 1;
  bool generic = false;
  std::vector<FieldLayout> fields;
};

// One variant's payload inside an enum. A unit variant has no fields and
// no payload bytes; every variant starts at the enum's payload offset.
struct EnumVariantLayout {
  std::string name;
  u32 tag = 0;
  u64 offset = 0;
  u64 size = 0;
  std::vector<FieldLayout> fields;
};

// The memory layout of one enum declaration: the discriminant, the
// payload area the widest variant reserves, and every variant's fields.
struct EnumLayout {
  std::string name;
  u64 size = 0;
  u64 align = 1;
  u64 discriminant_size = 4;
  u64 payload_offset = 0;
  u64 payload_size = 0;
  u64 payload_align = 1;
  bool generic = false;
  std::vector<EnumVariantLayout> variants;
};

// The layout of the struct or enum declaration whose name contains
// position. Each runs the frontend, so a caller that already holds a
// checked package uses the _of form below.
std::optional<StructLayout> struct_layout_at(const Document& document,
                                             Position position);
std::optional<EnumLayout> enum_layout_at(const Document& document,
                                         Position position);

// The same over a checked package the caller already holds, with the
// source the package was built from. Hover runs the frontend once and
// reads every layout through these.
std::optional<StructLayout> struct_layout_of(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    std::string_view source,
    ast::ItemIdx item);
std::optional<EnumLayout> enum_layout_of(
    const pipeline::PipelineContext& context,
    const analyzer::CheckedPackage& package,
    std::string_view source,
    ast::ItemIdx item);

// Markdown for a hover: the size and alignment, then a table of fields
// or variants.
std::string struct_layout_markdown(const StructLayout& layout);
std::string enum_layout_markdown(const EnumLayout& layout);

// The layout as the alcy/structLayout result object.
text::JsonValue struct_layout_json(const StructLayout& layout);

}  // namespace lsp
