// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "linter/level.h"
#include "linter/rule.h"
#include "source/source.h"

namespace linter {

// Configuration for the goal package. The file is optional; absent
// means every rule keeps its default level and options. See
// docs/adr/0082-the-linter-is-a-library-over-the-frontend.md.
constexpr std::string_view LINT_FILE_NAME = "lint.toml";

// One rule's resolved settings. The level may be Allow even though the
// rule registered warn by default; that is what silencing it means.
struct RuleSetting {
  LintLevel level = LintLevel::Warn;
  // The most parameters a declaration may carry before
  // too-many-arguments reports it. Zero keeps the rule's own default.
  u32 max_args = 0;
  // Names the rule must not report, from the ignore option. The views
  // are arena-owned by the caller and live as long as the run; empty
  // means the rule filters nothing.
  std::span<const std::string_view> ignore;
};

// The resolved configuration for one invocation: a default level and
// one setting per registered rule, in registry order so a lookup is an
// index. The rules span is arena-owned by the caller and lives as long
// as the run.
struct LintConfig {
  LintLevel default_level = LintLevel::Warn;
  std::span<const RuleSetting> rules;
  // Whether the frontend's lowering and borrow tail runs after the
  // rules. The rules read the analyzed package and do not need it; the
  // tail is what makes a lint run reject a program the compiler would.
  // False trades that verdict for about half the run.
  bool borrow_checks = true;

  // The setting for the rule at the given index in the registry. An
  // index past the table reads the rule's own default: a config that
  // names nothing - the lsp's, or a test's - is not a config that
  // turned every rule up to `level`, and a rule registered after this
  // table was parsed is configured by its default.
  [[nodiscard]] RuleSetting setting(u32 index) const {
    if (index < rules.size()) {
      return rules[index];
    }
    const std::span<const Rule> all = all_rules();
    if (index < all.size()) {
      return RuleSetting{.level = all[index].default_level, .ignore = {}};
    }
    return RuleSetting{.level = default_level, .ignore = {}};
  }
};

// Parses .alcy/lint.toml bytes. Unknown keys are ignored, like
// alcy.toml; a level that names no level and a rule name that matches
// no registered rule are errors, because both are typos a reader wants
// to hear about rather than silence.
base::Result<LintConfig, diag::Reported> parse_lint_config(
    std::string_view bytes,
    std::string_view filename,
    source::FileId /*file*/,
    diag::DiagBag& bag,
    mem::Arena& arena);

}  // namespace linter
