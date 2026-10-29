// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "linter/context.h"
#include "linter/rules/rules.h"

namespace linter::rules {

namespace {

// The parameter count above which a declaration is hard to call. Every
// rule can be given this in lint.toml through too-many-arguments;
// seven is the default the option falls back to.
constexpr u32 DEFAULT_MAX_ARGS = 7;

// Whether a kind is a declaration a reader calls by name and can
// therefore expect a doc comment on. An impl block is addressed through
// its methods; a use names something else's declaration; a compiler
// intrinsic has no source to document.
bool documented_kind(ast::ItemKind kind) {
  using K = ast::ItemKind;
  switch (kind) {
    case K::Fn:
    case K::Struct:
    case K::Enum:
    case K::Spec:
    case K::Static:
    case K::Const: return true;
    default: return false;
  }
}

void check_missing_docs(LintContext& ctx, ast::ItemIdx item) {
  const ast::ItemNode& node = ctx.ast.items[item];
  if (!node.is_pub || !node.docs.empty() || !documented_kind(node.kind)) {
    return;
  }
  ctx.report<i18n::Key::LintMissingDocs>(node.span, node.name());
}

void check_too_many_arguments(LintContext& ctx, ast::ItemIdx item) {
  const ast::ItemNode& node = ctx.ast.items[item];
  if (node.kind != ast::ItemKind::Fn) {
    return;
  }
  const ast::ItemFn& fn = node.payload.get<ast::ItemFn>();
  u32 count = 0;
  for (const ast::ItemFnParam& param : fn.params) {
    if (!param.is_variadic) {
      ++count;
    }
  }
  const u32 configured = ctx.setting().max_args;
  const u32 limit = configured == 0 ? DEFAULT_MAX_ARGS : configured;
  if (count > limit) {
    ctx.report<i18n::Key::LintTooManyArguments>(fn.name.span, fn.name.name,
                                                count, limit);
  }
}

}  // namespace

void MissingDocs::check_item(LintContext& ctx, ast::ItemIdx item) {
  check_missing_docs(ctx, item);
}

void TooManyArguments::check_item(LintContext& ctx, ast::ItemIdx item) {
  check_too_many_arguments(ctx, item);
}

}  // namespace linter::rules
