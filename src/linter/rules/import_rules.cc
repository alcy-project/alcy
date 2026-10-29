// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "linter/context.h"
#include "linter/rules/rules.h"
#include "linter/usage.h"

namespace linter::rules {

namespace {

// The local name a use introduces, and the span a reader looks at.
std::string_view local_name(const LintContext& ctx,
                            const ast::ItemUse& use,
                            diag::Span* span) {
  if (use.has_alias) {
    *span = use.alias.span;
    return use.alias.name;
  }
  if (!use.path.is_valid()) {
    return {};
  }
  const ast::Path& path = ctx.ast.paths[use.path];
  if (path.segments.empty()) {
    return {};
  }
  *span = path.segments.back().span;
  return path.segments.back().name;
}

// The name a use's own path references, when it references one: a
// multi-segment path is rooted in something already in scope, and so is
// a one-segment path that renames what it names. A one-segment use with
// no alias is a declaration and references nothing.
std::string_view referenced_head(const LintContext& ctx,
                                 const ast::ItemUse& use) {
  if (!use.path.is_valid()) {
    return {};
  }
  const ast::Path& path = ctx.ast.paths[use.path];
  if (path.segments.empty() || (path.segments.size() == 1 && !use.has_alias)) {
    return {};
  }
  return path.segments.front().name;
}

}  // namespace

void UnusedImport::run(LintContext& ctx) {
  if (ctx.usage_index == nullptr) {
    return;
  }
  // The heads the walk recorded. The imports are few, so sorting once
  // and binary-searching each name is cheaper than a hash table.
  std::vector<std::string_view> used = ctx.usage_index->names;
  std::sort(used.begin(), used.end());
  used.erase(std::unique(used.begin(), used.end()), used.end());

  struct Use {
    ast::ItemIdx item;
    ast::ItemUse use;
  };
  std::vector<Use> uses;
  const analyzer::ModuleNode& module = ctx.module_node();
  for (const ast::ItemIdx item : module.items) {
    const ast::ItemNode& node = ctx.ast.items[item];
    if (node.kind == ast::ItemKind::Use) {
      uses.push_back(Use{item, node.payload.get<ast::ItemUse>()});
    }
  }

  for (const Use& entry : uses) {
    const ast::ItemNode& node = ctx.ast.items[entry.item];
    // A public import re-exports what it names; the module's surface is
    // a use even when no path inside the module spells it.
    if (node.is_pub) {
      continue;
    }
    diag::Span name_span{};
    const std::string_view name = local_name(ctx, entry.use, &name_span);
    // An underscore alias imports for resolution's sake, not for a name
    // the module reads.
    if (name.empty() || name == "_") {
      continue;
    }
    const std::span<const std::string_view> ignore = ctx.setting().ignore;
    if (std::find(ignore.begin(), ignore.end(), name) != ignore.end()) {
      continue;
    }
    const bool from_path = std::binary_search(used.begin(), used.end(), name);
    bool from_another_use = false;
    for (const Use& other : uses) {
      if (other.item == entry.item) {
        continue;
      }
      if (referenced_head(ctx, other.use) == name) {
        from_another_use = true;
        break;
      }
    }
    if (from_path || from_another_use) {
      continue;
    }
    const u32 finding =
        ctx.report<i18n::Key::LintUnusedImport>(name_span, name);
    // The repair removes the whole item and the newline that ends it
    // when it is alone on its line, so the fix leaves no blank line.
    diag::Span removal = node.span;
    const std::optional<std::string_view> bytes =
        ctx.sources.bytes(node.span.file);
    if (bytes.has_value()) {
      const usize end = node.span.offset + node.span.length;
      if (end < bytes->size() && (*bytes)[end] == '\n') {
        removal.length += 1;
      }
    }
    ctx.fix(finding, removal, "");
  }
}

}  // namespace linter::rules
