// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "linter/context.h"
#include "linter/rules/rules.h"
#include "linter/usage.h"

namespace linter::rules {

namespace {

// One enum variant the rule may report: the declaration's name span,
// the variant, and the enum it belongs to.
struct Variant {
  std::string_view name;
  std::string_view enum_name;
  diag::Span span;
};

}  // namespace

void UnusedVariant::run(LintContext& ctx) {
  if (ctx.variant_usage == nullptr) {
    return;
  }
  // The variant names the package constructs. The list is small, so
  // sorting once and searching each candidate is cheaper than a hash
  // table.
  std::vector<std::string_view> used = ctx.variant_usage->names;
  std::sort(used.begin(), used.end());

  // The variants of the module's private enums. A pub enum is the
  // package's surface: another package may construct every variant, and
  // this run does not read them.
  std::vector<Variant> variants;
  for (const ast::ItemIdx item : ctx.module_node().items) {
    const ast::ItemNode& node = ctx.ast.items[item];
    if (node.kind != ast::ItemKind::Enum || node.is_pub) {
      continue;
    }
    const ast::ItemEnum& declaration = node.payload.get<ast::ItemEnum>();
    for (const ast::ItemEnumVariant& variant : declaration.variants) {
      variants.push_back(Variant{.name = variant.name.name,
                                 .enum_name = declaration.name.name,
                                 .span = variant.name.span});
    }
  }

  for (const Variant& variant : variants) {
    if (std::binary_search(used.begin(), used.end(), variant.name)) {
      continue;
    }
    ctx.report<i18n::Key::LintUnusedVariant>(variant.span, variant.name,
                                             variant.enum_name);
  }
}

}  // namespace linter::rules
