// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <span>
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

// One struct field the rule may report: the declaration's name span and
// the struct it belongs to.
struct Field {
  std::string_view name;
  std::string_view struct_name;
  diag::Span span;
};

}  // namespace

void UnusedField::run(LintContext& ctx) {
  if (ctx.field_usage == nullptr) {
    return;
  }
  // The engine sorted the field names once before any rule ran.
  const std::span<const std::string_view> read = ctx.field_usage->names;

  // The struct fields the module declares. A pub field is the package's
  // surface; the struct's own pub does not make every field one.
  std::vector<Field> fields;
  for (const ast::ItemIdx item : ctx.module_node().items) {
    const ast::ItemNode& node = ctx.ast.items[item];
    if (node.kind != ast::ItemKind::Struct) {
      continue;
    }
    // A pub field is the package's surface even on a private struct (a
    // nested module may read it); the struct's own pub does not make
    // every field one.
    const ast::ItemStruct& declaration = node.payload.get<ast::ItemStruct>();
    for (const ast::ItemStructField& field : declaration.fields) {
      if (field.is_pub) {
        continue;
      }
      fields.push_back(Field{.name = field.name.name,
                             .struct_name = declaration.name.name,
                             .span = field.name.span});
    }
  }

  for (const Field& field : fields) {
    if (std::binary_search(read.begin(), read.end(), field.name)) {
      continue;
    }
    ctx.report<i18n::Key::LintUnusedField>(field.span, field.name,
                                           field.struct_name);
  }
}

}  // namespace linter::rules
