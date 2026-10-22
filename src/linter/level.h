// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string_view>

#include "fpag/base/numeric.h"

namespace linter {

// How a lint finding is treated. The three levels are the conventional
// ones: allow silences a rule, warn reports it without failing the run,
// and deny reports it and fails the run. A level is never inferred from
// the finding; it comes from lint.toml or the rule's default.
enum class LintLevel : u8 {
  Allow,
  Warn,
  Deny,
};

// The spelling a lint.toml level takes. Nothing for a name that is not
// one, so a caller can report the typo rather than silently defaulting.
std::optional<LintLevel> lint_level_from_name(std::string_view name);

// The same spelling, so a diagnostic and the file it came from agree.
std::string_view lint_level_name(LintLevel level);

}  // namespace linter
