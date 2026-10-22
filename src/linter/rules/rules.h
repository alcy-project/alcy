// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "linter/context.h"

// One entry point per rule. The registry in rule.cc pairs each with its
// id, default level, and code; the definitions are grouped by what they
// read, so a rule that only walks nodes sits beside the others that do.
namespace linter::rules {

// Rules over the binding analysis: a name and what its scope does with it.
void unused_variable(LintContext& ctx);
void unused_mut(LintContext& ctx);
void shadow(LintContext& ctx);

// Rules over control flow: blocks, loops, and returns.
void unreachable_code(LintContext& ctx);
void while_true(LintContext& ctx);
void empty_loop(LintContext& ctx);
void needless_return(LintContext& ctx);

// Rules over expressions.
void double_negation(LintContext& ctx);
void self_comparison(LintContext& ctx);
void nested_if(LintContext& ctx);

// Rules over item declarations.
void missing_docs(LintContext& ctx);
void too_many_arguments(LintContext& ctx);

}  // namespace linter::rules
