// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>
#include <concepts>
#include <span>
#include <string_view>

#include "fpag/base/numeric.h"
#include "linter/diag_code.h"
#include "linter/level.h"

namespace linter {

struct LintContext;

// One check the linter runs, as a type: the identity lint.toml
// addresses and the check itself live in one place, and this concept
// holds every rule to the shape, so a rule that forgets a field or
// misspells one fails here instead of at the registry.
template <typename R>
concept RuleType = requires(LintContext& ctx) {
  { R::ID } -> std::convertible_to<std::string_view>;
  { R::DEFAULT_LEVEL } -> std::convertible_to<LintLevel>;
  { R::CODE } -> std::convertible_to<LintCode>;
  { R::SUMMARY } -> std::convertible_to<std::string_view>;
  R::run(ctx);
};

// The row the runtime reads. A rule type compiles to one of these in
// the registry's table: the engine iterates rows and lint.toml's rule
// names resolve against them, so the dispatch is one function pointer
// per rule whatever the type carries.
struct Rule {
  std::string_view id;
  LintLevel default_level;
  LintCode code;
  // One line describing what the rule reports, for --help and the
  // module's README. Not the message a finding carries.
  std::string_view summary;
  void (*run)(LintContext&);
};

template <RuleType R>
constexpr Rule rule_row() {
  return Rule{
      .id = R::ID,
      .default_level = R::DEFAULT_LEVEL,
      .code = R::CODE,
      .summary = R::SUMMARY,
      .run = &R::run,
  };
}

// Folds a list of rule types into the runtime table. The list's order
// is the table's order, which is the index the parsed configuration is
// aligned with, so a rule is appended and never inserted.
template <RuleType... Rs>
constexpr std::array<Rule, sizeof...(Rs)> rule_table() {
  return {{rule_row<Rs>()...}};
}

// Every rule, in registration order.
std::span<const Rule> all_rules();

// The registry index of a rule, or one past the end for a name no rule
// uses.
constexpr u32 NO_RULE = ~u32{0};

u32 rule_index(std::string_view id);

// The rule a name selects, or nullptr.
const Rule* find_rule(std::string_view id);

}  // namespace linter
