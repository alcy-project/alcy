// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// One field of a struct memory layout: where it starts, and the size and
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

// The layout of the struct whose declaration name contains position, or
// nullopt when the position is on no struct declaration. Runs the
// frontend to the checked package, so the caller decides when that cost
// is worth paying.
std::optional<StructLayout> struct_layout_at(const Document& document,
                                             Position position);

// Markdown for a hover: the size and alignment, then a table of fields.
std::string struct_layout_markdown(const StructLayout& layout);

// The layout as the alcy/structLayout result object.
text::JsonValue struct_layout_json(const StructLayout& layout);

}  // namespace lsp
