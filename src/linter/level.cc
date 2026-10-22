// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/level.h"

#include <optional>
#include <string_view>

namespace linter {

std::optional<LintLevel> lint_level_from_name(std::string_view name) {
  if (name == "allow") {
    return LintLevel::Allow;
  }
  if (name == "warn") {
    return LintLevel::Warn;
  }
  if (name == "deny") {
    return LintLevel::Deny;
  }
  return std::nullopt;
}

std::string_view lint_level_name(LintLevel level) {
  switch (level) {
    case LintLevel::Allow: return "allow";
    case LintLevel::Warn: return "warn";
    case LintLevel::Deny: return "deny";
  }
  return "allow";
}

}  // namespace linter
