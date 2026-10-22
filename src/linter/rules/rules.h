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
  static void run(LintContext& ctx);
};

struct UnusedMut {
  static constexpr std::string_view ID = "unused-mut";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::UnusedMut;
  static constexpr std::string_view SUMMARY =
      "a mut binding that is never written";
  static void run(LintContext& ctx);
};

struct Shadow {
  static constexpr std::string_view ID = "shadow";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Allow;
  static constexpr LintCode CODE = LintCode::Shadow;
  static constexpr std::string_view SUMMARY =
      "a binding that shadows one in an enclosing scope";
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
  static void run(LintContext& ctx);
};

struct WhileTrue {
  static constexpr std::string_view ID = "while-true";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::WhileTrue;
  static constexpr std::string_view SUMMARY = "a while over the literal true";
  static void run(LintContext& ctx);
};

struct EmptyLoop {
  static constexpr std::string_view ID = "empty-loop";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::EmptyLoop;
  static constexpr std::string_view SUMMARY = "a loop whose body is empty";
  static void run(LintContext& ctx);
};

struct NeedlessReturn {
  static constexpr std::string_view ID = "needless-return";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::NeedlessReturn;
  static constexpr std::string_view SUMMARY =
      "a return that ends a function body";
  static void run(LintContext& ctx);
};

// Rules over expressions.
struct DoubleNegation {
  static constexpr std::string_view ID = "double-negation";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::DoubleNegation;
  static constexpr std::string_view SUMMARY = "an operator applied to itself";
  static void run(LintContext& ctx);
};

struct SelfComparison {
  static constexpr std::string_view ID = "self-comparison";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::SelfComparison;
  static constexpr std::string_view SUMMARY =
      "a comparison whose operands are the same expression";
  static void run(LintContext& ctx);
};

struct NestedIf {
  static constexpr std::string_view ID = "nested-if";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::NestedIf;
  static constexpr std::string_view SUMMARY =
      "an if whose only statement is another if";
  static void run(LintContext& ctx);
};

// Rules over item declarations.
struct MissingDocs {
  static constexpr std::string_view ID = "missing-docs";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Allow;
  static constexpr LintCode CODE = LintCode::MissingDocs;
  static constexpr std::string_view SUMMARY =
      "a public item without a doc comment";
  static void run(LintContext& ctx);
};

struct TooManyArguments {
  static constexpr std::string_view ID = "too-many-arguments";
  static constexpr LintLevel DEFAULT_LEVEL = LintLevel::Warn;
  static constexpr LintCode CODE = LintCode::TooManyArguments;
  static constexpr std::string_view SUMMARY =
      "a function taking more parameters than the limit";
  static void run(LintContext& ctx);
};

}  // namespace linter::rules
