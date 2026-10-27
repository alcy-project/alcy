// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/protocol.h"

#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

text::JsonValue position_json(Position position) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("line", text::JsonValue::make_number(position.line));
  out.set("character", text::JsonValue::make_number(position.character));
  return out;
}

text::JsonValue range_json(const Document& document, u32 offset, u32 length) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("start",
          position_json(document.index().position_at(document.text(), offset)));
  out.set("end", position_json(document.index().position_at(
                     document.text(), static_cast<usize>(offset) + length)));
  return out;
}

}  // namespace lsp
