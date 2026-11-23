// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/completion.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_display.h"
#include "lexer/lexer.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "text/json.h"

namespace lsp {

namespace {

struct Item {
  std::string label;
  std::string detail;
  ast::ItemIdx declaration = ast::ItemIdx::invalid();
  CompletionItemKind kind = CompletionItemKind::Variable;
  // A variant is shown by its short name but inserted qualified, as the
  // language requires at a use site.
  std::string insert_text;
};

std::string_view prefix_at(std::string_view text, usize offset) {
  usize start = offset;
  while (start > 0) {
    const char c = text[start - 1];
    // A qualified name is one prefix: replacing only its last segment
    // would double the qualifier for a variant item's insert text.
    const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == ':';
    if (!word) {
      break;
    }
    --start;
  }
  return text.substr(start, offset - start);
}

std::string function_detail(const pipeline::PipelineContext& context,
                            const ir::StorageState& state,
                            std::span<const ir::TypeIdx> params,
                            ir::TypeIdx ret,
                            std::string_view variadic_name) {
  std::string out = "(";
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
  return out;
}

// The names the root module can actually spell: its own declarations and
// everything its `use` statements and the prelude brought into scope.
// For each module, the declared name -> the names the root module can
// spell for it. An `as` import contributes its alias, which is the only
// usable label for that declaration in the buffer.
using VisibleNames = std::unordered_map<
    u32,
    std::unordered_map<std::string_view, std::vector<std::string_view>>>;

VisibleNames visible_names(const DocumentAnalysis& analysis) {
  VisibleNames names;
  const analyzer::CheckedPackage& package = analysis.package();
  u32 root_module = analyzer::NO_MODULE;
  for (u32 i = 0; i < package.tree.modules.size(); ++i) {
    if (package.tree.modules[i]->file == analysis.root()) {
      root_module = i;
      break;
    }
  }
  if (root_module == analyzer::NO_MODULE) {
    return names;
  }
  for (const analyzer::Import& import :
       package.tree.modules[root_module]->imports) {
    names[import.target_module][import.member].push_back(import.name);
  }
  for (const analyzer::CheckedModule& module : package.modules) {
    if (module.module != root_module) {
      continue;
    }
    for (const analyzer::CheckedModule::FnSig& fn : module.functions) {
      names[root_module][fn.name].push_back(fn.name);
    }
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      names[root_module][method.name].push_back(method.name);
    }
    for (const analyzer::CheckedModule::NamedType& named : module.types) {
      names[root_module][named.name].push_back(named.name);
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      names[root_module][info.name].push_back(info.name);
    }
  }
  return names;
}

std::vector<std::string_view> visible_labels(const VisibleNames& visible,
                                             u32 module,
                                             std::string_view name) {
  const auto found = visible.find(module);
  if (found == visible.end()) {
    return {};
  }
  const auto labels = found->second.find(name);
  return labels == found->second.end() ? std::vector<std::string_view>{}
                                       : labels->second;
}

std::string doc_text(const DocumentAnalysis& analysis, ast::ItemIdx item) {
  if (!item.is_valid()) {
    return {};
  }
  const auto& context = analysis.context();
  const ast::ItemNode& node = context.ast.items[item];
  std::string text;
  for (const diag::Span span : node.docs) {
    const std::optional<std::string_view> bytes =
        context.sources.bytes(span.file);
    if (!bytes.has_value() || span.offset + span.length > bytes->size()) {
      continue;
    }
    std::string_view line = bytes->substr(span.offset, span.length);
    if (line.starts_with("///")) {
      line.remove_prefix(3);
    }
    if (line.starts_with(' ')) {
      line.remove_prefix(1);
    }
    if (!text.empty()) {
      text.push_back('\n');
    }
    text.append(line);
  }
  return text;
}

void collect(const DocumentAnalysis& analysis,
             const analyzer::CheckedPackage& package,
             const VisibleNames& visible,
             std::vector<Item>* out) {
  const pipeline::PipelineContext& context = analysis.context();
  const ir::StorageState& state = analysis.types();
  const auto emit = [&](u32 module, ast::ItemIdx item,
                        std::string_view declared, std::string_view detail,
                        CompletionItemKind kind) {
    for (const std::string_view label :
         visible_labels(visible, module, declared)) {
      out->push_back(
          Item{std::string(label), std::string(detail), item, kind, ""});
    }
  };
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& fn : module.functions) {
      emit(module.module, fn.item, fn.name,
           function_detail(context, state, fn.params, fn.ret,
                           variadic_param_name(context, fn.item)),
           fn.is_method ? CompletionItemKind::Method
                        : CompletionItemKind::Function);
    }
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      // The definition-site check registers rows for a parameter's
      // bound ; they name no declaration, so there is
      // nothing here to offer.
      if (!method.item.is_valid()) {
        continue;
      }
      emit(module.module, method.item, method.name,
           function_detail(context, state, method.params, method.ret,
                           variadic_param_name(context, method.item)),
           CompletionItemKind::Method);
    }
    for (const analyzer::CheckedModule::NamedType& named : module.types) {
      const ir::TypeNode& type = state.types[named.type];
      ast::ItemIdx declaration = ast::ItemIdx::invalid();
      if (type.tag == ir::TypeTag::Enum) {
        for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
          if (info.type == named.type) {
            declaration = info.item;
            break;
          }
        }
      } else {
        for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
          if (info.type == named.type) {
            declaration = info.item;
            break;
          }
        }
      }
      emit(module.module, declaration, named.name,
           ir::type_display(state, context.strings, named.type),
           type.tag == ir::TypeTag::Enum ? CompletionItemKind::Enum
                                         : CompletionItemKind::Struct);
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      const auto labels = visible_labels(visible, module.module, info.name);
      if (labels.empty()) {
        continue;
      }
      // A use alias is what the buffer can spell; inserting the
      // declaration name would be unresolved at the use site.
      const std::string qualifier(labels.front());
      for (const std::string_view variant : info.variants) {
        Item item{std::string(variant), qualifier + "::" + std::string(variant),
                  ast::ItemIdx::invalid(), CompletionItemKind::EnumMember, ""};
        item.insert_text = item.detail;
        out->push_back(std::move(item));
      }
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      emit(module.module, info.item, info.name,
           ir::type_display(state, context.strings, info.type),
           info.is_const ? CompletionItemKind::Constant
                         : CompletionItemKind::Variable);
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
  const usize qualifier_end = prefix.rfind("::");
  const std::string_view name_prefix = qualifier_end == std::string_view::npos
                                           ? prefix
                                           : prefix.substr(qualifier_end + 2);

  std::vector<Item> items;
  if (analysis.ok()) {
    collect(analysis, analysis.package(), visible_names(analysis), &items);
  }
  for (const lexer::KeywordInfo& keyword : lexer::keyword_infos()) {
    if (keyword.category != lexer::KeywordCategory::Keyword) {
      continue;
    }
    items.push_back(Item{std::string(keyword.spelling), "",
                         ast::ItemIdx::invalid(), CompletionItemKind::Keyword,
                         ""});
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
    if (!starts_with(item.label, name_prefix)) {
      continue;
    }
    text::JsonValue entry = text::JsonValue::make_object();
    entry.set("label", text::JsonValue::make_string(item.label));
    entry.set("kind",
              text::JsonValue::make_number(static_cast<f64>(item.kind)));
    if (!item.detail.empty()) {
      entry.set("detail", text::JsonValue::make_string(item.detail));
    }
    std::string documentation = doc_text(analysis, item.declaration);
    if (!documentation.empty()) {
      text::JsonValue doc = text::JsonValue::make_object();
      doc.set("kind", text::JsonValue::make_string("markdown"));
      doc.set("value", text::JsonValue::make_string(documentation));
      entry.set("documentation", std::move(doc));
    }
    text::JsonValue edit_range =
        range_json(document, static_cast<u32>(offset - prefix.size()),
                   static_cast<u32>(prefix.size()));
    text::JsonValue text_edit = text::JsonValue::make_object();
    text_edit.set("range", std::move(edit_range));
    text_edit.set("newText", text::JsonValue::make_string(
                                 item.insert_text.empty() ? item.label
                                                          : item.insert_text));
    entry.set("textEdit", std::move(text_edit));
    array.push(std::move(entry));
  }

  text::JsonValue list = text::JsonValue::make_object();
  list.set("isIncomplete", text::JsonValue::make_bool(false));
  list.set("items", std::move(array));
  return list;
}

}  // namespace lsp
