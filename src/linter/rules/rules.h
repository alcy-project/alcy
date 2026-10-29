// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "linter/context.h"
#include "linter/diag_code.h"
#include "linter/level.h"

// One rule is a type: the identity lint.toml addresses as static
// members, the check as a static run. The concept in rule.h holds each
// to the shape, the registry folds the type list into the runtime
// table, and a rule's definition sits in the group translation unit
// beside the checks it drives.
namespace linter::rules {

// Rules over the binding analysis: a name and what its scope does with it.
struct UnusedVariable {
  static constexpr std::string_view ID = "unused-variable";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::UnusedVariable;
  static constexpr std::string_view SUMMARY = "a binding no expression reads";
  static constexpr bool READS_BINDINGS = true;
  static void run(LintContext& ctx);
};

struct UnusedMut {
  static constexpr std::string_view ID = "unused-mut";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::UnusedMut;
  static constexpr std::string_view SUMMARY =
      "a mut binding that is never written";
  static constexpr bool READS_BINDINGS = true;
  static void run(LintContext& ctx);
};

struct Shadow {
  static constexpr std::string_view ID = "shadow";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Allow;
  static constexpr LintCode CODE = LintCode::Shadow;
  static constexpr std::string_view SUMMARY =
      "a binding that shadows one in an enclosing scope";
  static constexpr bool READS_BINDINGS = true;
  static void run(LintContext& ctx);
};

// Rules over control flow: blocks, loops, and returns. Unreachable code
// is opt-in: the lowering pass already warns about the same expression
// (WD003), and the lint exists for a caller that lints without lowering.
struct UnreachableCode {
  static constexpr std::string_view ID = "unreachable-code";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Allow;
  static constexpr LintCode CODE = LintCode::UnreachableCode;
  static constexpr std::string_view SUMMARY =
      "a statement after control leaves the block";
  static void check_block(LintContext& ctx, ast::BlockIdx block);
};

struct WhileTrue {
  static constexpr std::string_view ID = "while-true";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::WhileTrue;
  static constexpr std::string_view SUMMARY = "a while over the literal true";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

struct EmptyLoop {
  static constexpr std::string_view ID = "empty-loop";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::EmptyLoop;
  static constexpr std::string_view SUMMARY = "a loop whose body is empty";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

// Rules over expressions.
struct DoubleNegation {
  static constexpr std::string_view ID = "double-negation";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::DoubleNegation;
  static constexpr std::string_view SUMMARY = "an operator applied to itself";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

struct SelfComparison {
  static constexpr std::string_view ID = "self-comparison";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::SelfComparison;
  static constexpr std::string_view SUMMARY =
      "a comparison whose operands are the same expression";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

struct NestedIf {
  static constexpr std::string_view ID = "nested-if";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::NestedIf;
  static constexpr std::string_view SUMMARY =
      "an if whose only statement is another if";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

// Rules over the checked types: the analyzer's answer for an
// expression rather than the spelling the source wrote.
struct UselessCast {
  static constexpr std::string_view ID = "useless-cast";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::UselessCast;
  static constexpr std::string_view SUMMARY =
      "a cast to the type the value already has";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

struct FloatEquality {
  static constexpr std::string_view ID = "float-equality";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Allow;
  static constexpr LintCode CODE = LintCode::FloatEquality;
  static constexpr std::string_view SUMMARY =
      "an equality test on floating-point values";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

struct NeedlessBorrow {
  static constexpr std::string_view ID = "needless-borrow";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::NeedlessBorrow;
  static constexpr std::string_view SUMMARY =
      "a borrow of a dereference that is the reference itself";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

struct LossyCast {
  static constexpr std::string_view ID = "lossy-cast";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Allow;
  static constexpr LintCode CODE = LintCode::LossyCast;
  static constexpr std::string_view SUMMARY =
      "a cast that narrows the value's scalar type";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

struct BoolComparison {
  static constexpr std::string_view ID = "bool-comparison";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::BoolComparison;
  static constexpr std::string_view SUMMARY =
      "a comparison of a bool with a literal";
  static void check_expr(LintContext& ctx, ast::ExprIdx expr);
};

// Rules over item declarations.
struct MissingDocs {
  static constexpr std::string_view ID = "missing-docs";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Allow;
  static constexpr LintCode CODE = LintCode::MissingDocs;
  static constexpr std::string_view SUMMARY =
      "a public item without a doc comment";
  static void check_item(LintContext& ctx, ast::ItemIdx item);
};

struct TooManyArguments {
  static constexpr std::string_view ID = "too-many-arguments";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::TooManyArguments;
  static constexpr std::string_view SUMMARY =
      "a function taking more parameters than the limit";
  static void check_item(LintContext& ctx, ast::ItemIdx item);
};

// A module rule: it reads the name-usage table the engine records during
// its walk and reports the imports no path names.
struct UnusedImport {
  static constexpr std::string_view ID = "unused-import";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::UnusedImport;
  static constexpr std::string_view SUMMARY =
      "an import the module never names";
  static constexpr bool READS_USAGE = true;
  static void run(LintContext& ctx);
};

}  // namespace linter::rules
