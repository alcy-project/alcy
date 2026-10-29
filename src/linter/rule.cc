// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/rule.h"

#include <span>
#include <string_view>

#include "fpag/base/numeric.h"
#include "linter/rules/rules.h"

namespace linter {

namespace {

// The registry. The type list is the whole act of registering: each
// type satisfied the concept before this compiled, and the table's
// order - the index the parsed configuration is aligned with - is this
// list's order, so a rule is appended and never inserted.
constexpr auto RULES = rule_table<rules::UnusedVariable,
                                  rules::UnusedMut,
                                  rules::Shadow,
                                  rules::UnreachableCode,
                                  rules::WhileTrue,
                                  rules::EmptyLoop,
                                  rules::DoubleNegation,
                                  rules::SelfComparison,
                                  rules::NestedIf,
                                  rules::UselessCast,
                                  rules::FloatEquality,
                                  rules::NeedlessBorrow,
                                  rules::LossyCast,
                                  rules::MissingDocs,
                                  rules::TooManyArguments,
                                  rules::UnusedImport,
                                  rules::BoolComparison,
                                  rules::DeadCode>();

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
