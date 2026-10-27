// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/formatting.h"

#include <string>
#include <string_view>
#include <utility>

#include "fmt/formatter.h"
#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "text/json.h"

namespace lsp {

text::JsonValue document_formatting(const Document& document) {
  text::JsonValue edits = text::JsonValue::make_array();
  std::string formatted =
      alcy_fmt::format(document.text(), alcy_fmt::default_format_config());
  if (std::string_view(formatted) == document.text()) {
    return edits;
  }
  const Position end =
      document.index().position_at(document.text(), document.text().size());

  text::JsonValue range = text::JsonValue::make_object();
  range.set("start", position_json(Position{0, 0}));
  range.set("end", position_json(end));

  text::JsonValue edit = text::JsonValue::make_object();
  edit.set("range", std::move(range));
  edit.set("newText", text::JsonValue::make_string(std::move(formatted)));
  edits.push(std::move(edit));
  return edits;
}

}  // namespace lsp
