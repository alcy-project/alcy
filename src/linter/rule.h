// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>
#include <concepts>
#include <span>
#include <string_view>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "linter/diag_code.h"
#include "linter/level.h"

namespace linter {

struct LintContext;

// A rule that reads the module's binding table declares the fact, so the
// engine builds the table once and the three rules over a name share it.
// A rule without the member does not read the table.
template <typename R>
concept ReadsBindings = requires {
  { R::READS_BINDINGS } -> std::convertible_to<bool>;
} && R::READS_BINDINGS;

// The identity every rule carries.
template <typename R>
concept RuleIdentity = requires {
  { R::ID } -> std::convertible_to<std::string_view>;
  { R::DEFAULT_LEVEL } -> std::convertible_to<LintLevel>;
  { R::CODE } -> std::convertible_to<LintCode>;
  { R::SUMMARY } -> std::convertible_to<std::string_view>;
};

// A rule states one check shape. A module rule runs itself and walks
// what it needs; an expression or item rule is a hook the engine's
// single shared traversal dispatches to, so rules over the same node
// family do not each walk the syntax.
template <typename R>
concept ModuleRule = requires(LintContext& ctx) { R::run(ctx); };

template <typename R>
concept ExprRule =
    requires(LintContext& ctx, ast::ExprIdx expr) { R::check_expr(ctx, expr); };

template <typename R>
concept ItemRule =
    requires(LintContext& ctx, ast::ItemIdx item) { R::check_item(ctx, item); };

// One check the linter runs, as a type: the identity lint.toml
// addresses and the check itself live in one place, and this concept
// holds every rule to the shape, so a rule that forgets a field or
// misspells one fails here instead of at the registry.
template <typename R>
concept RuleType =
    RuleIdentity<R> && (ModuleRule<R> || ExprRule<R> || ItemRule<R>);

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
  // Exactly one of the three check shapes is set; the concept and the
  // static assertion in rule_row hold that.
  void (*run)(LintContext&);
  void (*check_expr)(LintContext&, ast::ExprIdx);
  void (*check_item)(LintContext&, ast::ItemIdx);
  // Whether the rule reads the binding table the engine builds once per
  // module, rather than walking the syntax itself.
  bool reads_bindings = false;
};

template <RuleType R>
constexpr Rule rule_row() {
  static_assert(
      (ModuleRule<R> ? 1 : 0) + (ExprRule<R> ? 1 : 0) + (ItemRule<R> ? 1 : 0) ==
          1,
      "a rule states exactly one check shape");
  Rule rule{};
  rule.id = R::ID;
  rule.default_level = R::DEFAULT_LEVEL;
  rule.code = R::CODE;
  rule.summary = R::SUMMARY;
  if constexpr (ModuleRule<R>) {
    rule.run = &R::run;
  }
  if constexpr (ExprRule<R>) {
    rule.check_expr = &R::check_expr;
  }
  if constexpr (ItemRule<R>) {
    rule.check_item = &R::check_item;
  }
  rule.reads_bindings = ReadsBindings<R>;
  return rule;
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
