// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string_view>

#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "linter/bindings.h"
#include "linter/context.h"
#include "linter/rules/rules.h"

namespace linter::rules {

namespace {

// Names a reader never wants flagged: the language's own receivers and
// any binding whose author already said it is deliberate with a leading
// underscore. The wildcard pattern binds nothing and does not reach
// here.
bool skipped_name(std::string_view name) {
  if (!name.empty() && name[0] == '_') {
    return true;
  }
  return name == "self" || name == "Self" || name == "super" ||
         name == "package";
}

// A shadowed binding carries the desugarer's freshening suffix; the
// reader wrote the part before it.
std::string_view source_spelling(std::string_view name) {
  const usize dollar = name.find('$');
  return dollar == std::string_view::npos ? name : name.substr(0, dollar);
}

void report_unused(LintContext& ctx, const BindingUse& binding) {
  if (binding.reads != 0 || skipped_name(binding.name)) {
    return;
  }
  if (binding.is_param) {
    ctx.report<i18n::Key::LintUnusedParameter>(binding.span,
                                               source_spelling(binding.name));
    return;
  }
  const u32 finding = ctx.report<i18n::Key::LintUnusedVariable>(
      binding.span, source_spelling(binding.name));
  ctx.label<i18n::Key::LintUnusedVariableHelp>(finding, binding.span);
}

void report_unused_mut(LintContext& ctx, const BindingUse& binding) {
  // A binding no one reads is unused-variable's finding; reporting the
  // mut beside it says the same thing twice.
  if (!binding.is_mut || binding.reads == 0 || binding.mutations != 0 ||
      skipped_name(binding.name)) {
    return;
  }
  ctx.report<i18n::Key::LintUnusedMut>(binding.span,
                                       source_spelling(binding.name));
}

void report_shadow(LintContext& ctx, const BindingUse& binding) {
  if (!binding.is_shadowed || skipped_name(binding.name)) {
    return;
  }
  ctx.report<i18n::Key::LintShadow>(binding.span,
                                    source_spelling(binding.name));
}

}  // namespace

void UnusedVariable::run(LintContext& ctx) {
  if (ctx.binding_index == nullptr) {
    return;
  }
  for (const BindingUse& binding : ctx.binding_index->bindings) {
    report_unused(ctx, binding);
  }
}

void UnusedMut::run(LintContext& ctx) {
  if (ctx.binding_index == nullptr) {
    return;
  }
  for (const BindingUse& binding : ctx.binding_index->bindings) {
    report_unused_mut(ctx, binding);
  }
}

void Shadow::run(LintContext& ctx) {
  if (ctx.binding_index == nullptr) {
    return;
  }
  for (const BindingUse& binding : ctx.binding_index->bindings) {
    report_shadow(ctx, binding);
  }
}

}  // namespace linter::rules
