// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/completion.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "lsp/typing.h"
#include "pipeline/pipeline_context.h"
#include "text/json.h"

namespace lsp {

namespace {

const char* const KEYWORDS[] = {
    "as",     "break",  "comp", "const",  "continue", "else",      "enum",
    "extern", "false",  "fn",   "if",     "impl",     "intrinsic", "let",
    "loop",   "match",  "mut",  "pub",    "ret",      "self",      "spec",
    "static", "struct", "true", "unsafe", "use",      "while",
};

struct Item {
  std::string label;
  std::string detail;
  CompletionItemKind kind = CompletionItemKind::Variable;
};

std::string_view prefix_at(std::string_view text, usize offset) {
  usize start = offset;
  while (start > 0) {
    const char c = text[start - 1];
    const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_';
    if (!word) {
      break;
    }
    --start;
  }
  return text.substr(start, offset - start);
}

std::string function_detail(const pipeline::PipelineContext& context,
                            const analyzer::CheckedPackage& package,
                            std::span<const ir::TypeIdx> params,
                            ir::TypeIdx ret) {
  const ir::StorageState& state = package.types->state();
  std::string out = "(";
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
  return out;
}

void collect(const pipeline::PipelineContext& context,
             const analyzer::CheckedPackage& package,
             std::vector<Item>* out) {
  const ir::StorageState& state = package.types->state();
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& fn : module.functions) {
      out->push_back(Item{std::string(fn.name),
                          function_detail(context, package, fn.params, fn.ret),
                          fn.is_method ? CompletionItemKind::Method
                                       : CompletionItemKind::Function});
    }
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      out->push_back(
          Item{std::string(method.name),
               function_detail(context, package, method.params, method.ret),
               CompletionItemKind::Method});
    }
    for (const analyzer::CheckedModule::NamedType& named : module.types) {
      const ir::TypeNode& type = state.types[named.type];
      out->push_back(Item{std::string(named.name),
                          type_name(state, context.strings, named.type),
                          type.tag == ir::TypeTag::Enum
                              ? CompletionItemKind::Enum
                              : CompletionItemKind::Struct});
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      for (const std::string_view variant : info.variants) {
        out->push_back(
            Item{std::string(variant),
                 std::string(info.name) + "::" + std::string(variant),
                 CompletionItemKind::EnumMember});
      }
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      out->push_back(Item{std::string(info.name),
                          type_name(state, context.strings, info.type),
                          info.is_const ? CompletionItemKind::Constant
                                        : CompletionItemKind::Variable});
    }
  }
}

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

text::JsonValue completion_items(const Document& document,
                                 const DocumentAnalysis& analysis,
                                 Position position) {
  const usize offset = document.index().byte_offset(document.text(), position);
  const std::string_view prefix = prefix_at(document.text(), offset);

  std::vector<Item> items;
  if (analysis.ok()) {
    collect(analysis.context(), analysis.package(), &items);
  }
  for (const char* keyword : KEYWORDS) {
    items.push_back(Item{keyword, "", CompletionItemKind::Keyword});
  }

  std::sort(items.begin(), items.end(),
            [](const Item& left, const Item& right) {
              return left.label < right.label;
            });
  items.erase(std::unique(items.begin(), items.end(),
                          [](const Item& left, const Item& right) {
                            return left.label == right.label;
                          }),
              items.end());

  text::JsonValue array = text::JsonValue::make_array();
  for (const Item& item : items) {
    if (!starts_with(item.label, prefix)) {
      continue;
    }
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("label", text::JsonValue::make_string(item.label));
    entry.set("kind",
              text::JsonValue::make_number(static_cast<f64>(item.kind)));
    if (!item.detail.empty()) {
      entry.set("detail", text::JsonValue::make_string(item.detail));
    }
    array.push(std::move(entry));
  }

  text::JsonValue list = text::JsonValue::make_object();
  list.set("isIncomplete", text::JsonValue::make_bool(false));
  list.set("items", std::move(array));
  return list;
}

}  // namespace lsp
