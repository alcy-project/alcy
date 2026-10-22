// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "fpag/base/numeric.h"
#include "linter/diag_code.h"
#include "linter/level.h"

namespace linter {

struct LintContext;

// One registered check. A rule is data plus a function pointer rather
// than a class: the linter has no runtime polymorphism, and a table of
// these is what the configuration, the docs, and the engine all read.
//
// The id is the name lint.toml addresses and the name a rendered
// finding carries, so it is a stable part of the interface. The code is
// the machine's handle on the same check.
struct Rule {
  std::string_view id;
  LintLevel default_level;
  LintCode code;
  // One line describing what the rule reports, for --help and the
  // module's README. Not the message a finding carries.
  std::string_view summary;
  void (*run)(LintContext&);
};

// Every rule, in registration order. The order is the index the
// configuration table is aligned with, so a rule is appended, never
// inserted or removed without renumbering the codes beside it.
std::span<const Rule> all_rules();

// The registry index of a rule, or one past the end for a name no rule
// uses.
constexpr u32 NO_RULE = ~u32{0};

u32 rule_index(std::string_view id);

// The rule a name selects, or nullptr.
const Rule* find_rule(std::string_view id);

}  // namespace linter
