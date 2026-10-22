// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/rule.h"

#include <span>
#include <string_view>

#include "fpag/base/numeric.h"
#include "linter/diag_code.h"
#include "linter/level.h"
#include "linter/rules/rules.h"

namespace linter {

namespace {

constexpr Rule RULES[] = {
    Rule{.id = "unused-variable",
         .default_level = LintLevel::Warn,
         .code = LintCode::UnusedVariable,
         .summary = "a binding no expression reads",
         .run = &rules::unused_variable},
    Rule{.id = "unused-mut",
         .default_level = LintLevel::Warn,
         .code = LintCode::UnusedMut,
         .summary = "a mut binding that is never written",
         .run = &rules::unused_mut},
    Rule{.id = "shadow",
         .default_level = LintLevel::Allow,
         .code = LintCode::Shadow,
         .summary = "a binding that shadows one in an enclosing scope",
         .run = &rules::shadow},
    // The lowering pass already warns about an unreachable expression
    // (WD003), so this is opt-in rather than a second report by default.
    // It exists for a caller that lints without lowering.
    Rule{.id = "unreachable-code",
         .default_level = LintLevel::Allow,
         .code = LintCode::UnreachableCode,
         .summary = "a statement after control leaves the block",
         .run = &rules::unreachable_code},
    Rule{.id = "while-true",
         .default_level = LintLevel::Warn,
         .code = LintCode::WhileTrue,
         .summary = "a while over the literal true",
         .run = &rules::while_true},
    Rule{.id = "empty-loop",
         .default_level = LintLevel::Warn,
         .code = LintCode::EmptyLoop,
         .summary = "a loop whose body is empty",
         .run = &rules::empty_loop},
    Rule{.id = "needless-return",
         .default_level = LintLevel::Warn,
         .code = LintCode::NeedlessReturn,
         .summary = "a return that ends a function body",
         .run = &rules::needless_return},
    Rule{.id = "double-negation",
         .default_level = LintLevel::Warn,
         .code = LintCode::DoubleNegation,
         .summary = "an operator applied to itself",
         .run = &rules::double_negation},
    Rule{.id = "self-comparison",
         .default_level = LintLevel::Warn,
         .code = LintCode::SelfComparison,
         .summary = "a comparison whose operands are the same expression",
         .run = &rules::self_comparison},
    Rule{.id = "nested-if",
         .default_level = LintLevel::Warn,
         .code = LintCode::NestedIf,
         .summary = "an if whose only statement is another if",
         .run = &rules::nested_if},
    Rule{.id = "missing-docs",
         .default_level = LintLevel::Allow,
         .code = LintCode::MissingDocs,
         .summary = "a public item without a doc comment",
         .run = &rules::missing_docs},
    Rule{.id = "too-many-arguments",
         .default_level = LintLevel::Warn,
         .code = LintCode::TooManyArguments,
         .summary = "a function taking more parameters than the limit",
         .run = &rules::too_many_arguments},
};

}  // namespace

std::span<const Rule> all_rules() {
  return RULES;
}

u32 rule_index(std::string_view id) {
  const std::span<const Rule> all = all_rules();
  for (u32 i = 0; i < static_cast<u32>(all.size()); ++i) {
    if (all[i].id == id) {
      return i;
    }
  }
  return NO_RULE;
}

const Rule* find_rule(std::string_view id) {
  const u32 index = rule_index(id);
  return index == NO_RULE ? nullptr : &all_rules()[index];
}

}  // namespace linter
