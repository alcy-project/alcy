// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace linter {

// One id per check the linter registers under diag::Stage::Lint. The
// numbers are stable in the sense the compiler's are: a rule keeps the
// id it was assigned, and a retired rule's number is not reused.
//
// The first ten are the linter's own failures rather than a rule's,
// because lint.toml is read before any rule runs and a config the
// linter refused has to be reported under some code. Rules start at ten
// so the two ranges never meet.
enum class LintCode : u8 {
  ConfigSyntaxError = 1,
  ConfigSemanticError = 2,

  UnusedVariable = 10,
  UnusedMut = 11,
  UnreachableCode = 12,
  WhileTrue = 13,
  DoubleNegation = 14,
  SelfComparison = 15,
  NestedIf = 16,
  NeedlessReturn = 17,
  MissingDocs = 18,
  TooManyArguments = 19,
  EmptyLoop = 20,
  Shadow = 21,
};

}  // namespace linter
