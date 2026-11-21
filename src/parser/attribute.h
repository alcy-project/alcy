// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"

namespace parser {

// The build context the `when` gate reads: the target the run builds for
// plus the invocation mode. Built once per run from the target the pipeline
// was told and the command that was asked for.
struct BuildFacts {
  std::string_view os = "unknown";
  std::string_view arch = "unknown";
  bool freestanding = false;
  // Whether this invocation builds a test: `alcy test` sets it, every
  // other command leaves it false. The gate name is `testing` at top
  // level, not under `target`, because mode is not a target property.
  bool testing = false;
};

// Maps a target triple to the gate's view of it. A triple the table does
// not know spells `unknown` everywhere, so an unrecognized target loses
// no platform arm silently: the `when` gate evaluates `unknown` as the
// value and the source's unknown arms are the ones that survive.
BuildFacts build_facts(std::string_view triple,
                       bool freestanding,
                       bool testing);

// What the `when(EXPR)` on an item evaluates to on this target.
enum class GateOutcome : u8 {
  Keep,
  // False on this target. The diagnostics the evaluation reports, if
  // any, stand alongside; the item is dropped either way.
  Drop,
};

// Evaluates the tokens inside `when(...)`; `inside` holds the tokens
// strictly between the parentheses and `bytes` the source they were
// read from. Emits the gate's diagnostics against `bag`. `facts` is
// this run's target.
GateOutcome evaluate_when(std::span<const lexer::Token> inside,
                          std::string_view bytes,
                          BuildFacts facts,
                          diag::DiagBag& bag);

}  // namespace parser
