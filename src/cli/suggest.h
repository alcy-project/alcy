// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "fpag/arg/command.h"
#include "fpag/base/numeric.h"

namespace cli {

// "Did you mean" matching for misspelled subcommands and long flags.
// Pure and infallible: no I/O, no configuration, and no allocation
// beyond the owned string a suggestion returns, so a caller computes one
// on an error path and renders it.
//
// Matching is two mechanisms in one ranking. An affix match - the input
// is a prefix or a suffix of the candidate, or the other way round -
// catches a truncated or over-specified name (`trace` for `time-trace`,
// `output-file` for `output`). Anything else needs a small
// Damerau-Levenshtein distance, which catches a transposition along with
// the usual substitutions and edits. Short flags are out of
// scope: one character carries nothing to match on.
bool is_affix_match(std::string_view input, std::string_view candidate);

// Damerau-Levenshtein distance with adjacent transposition, over bytes.
// The names it compares are ASCII; anything else passes through
// untouched and simply measures far.
usize edit_distance(std::string_view a, std::string_view b);

// The closest candidate to the input, or nothing when every candidate
// is an affix miss beyond the distance threshold. Deterministic: affix
// matches first, then smaller distance, then shorter name, then lexical
// order. Borrows the candidates.
std::optional<std::string_view> best_match(
    std::string_view input,
    std::span<const std::string_view> candidates);

// The closest subcommand name to a misspelled one, if any.
std::optional<std::string> suggest_subcommand(const arg::Command& root,
                                              std::string_view name);

// The closest long flag to a misspelled one, if any. `args` is the raw
// invocation with element 0 the program name; the selected subcommand,
// when one is named exactly, scopes the candidates to the flags that
// command accepts plus the root globals. A flag typed before its
// subcommand resolves against the root scope alone, so the suggestion
// may name a flag that belongs to the selected command; the name is
// still the closest one.
std::optional<std::string> suggest_flag(const arg::Command& root,
                                        std::span<const std::string_view> args,
                                        std::string_view key);

// One suggestion for a single parse error, naming the error by its index
// into the outcome's error vector. The flag is the long name without
// dashes; rendering adds them.
struct FlagSuggestion {
  usize error_index;
  std::string flag;
};

}  // namespace cli
