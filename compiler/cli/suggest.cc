// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/suggest.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fpag/arg/command.h"
#include "fpag/base/numeric.h"

namespace cli {

bool is_affix_match(std::string_view input, std::string_view candidate) {
  return candidate.starts_with(input) || candidate.ends_with(input) ||
         input.starts_with(candidate) || input.ends_with(candidate);
}

usize edit_distance(std::string_view a, std::string_view b) {
  if (a == b) {
    return 0;
  }
  if (a.empty()) {
    return b.size();
  }
  if (b.empty()) {
    return a.size();
  }
  // Optimal string alignment: the full matrix for adjacent transposition,
  // three rows of it. The candidates are flag names, so the short side
  // is a handful of bytes and even a hostile input stays linear in it.
  std::vector<usize> previous(b.size() + 1);
  std::vector<usize> current(b.size() + 1);
  std::vector<usize> before_previous(b.size() + 1);
  for (usize j = 0; j <= b.size(); ++j) {
    previous[j] = j;
  }
  for (usize i = 1; i <= a.size(); ++i) {
    current[0] = i;
    for (usize j = 1; j <= b.size(); ++j) {
      const usize substitution =
          previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      usize best =
          std::min({previous[j] + 1, current[j - 1] + 1, substitution});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
        best = std::min(best, before_previous[j - 2] + 1);
      }
      current[j] = best;
    }
    before_previous.swap(previous);
    previous.swap(current);
  }
  return previous[b.size()];
}

std::optional<std::string_view> best_match(
    std::string_view input,
    std::span<const std::string_view> candidates) {
  if (input.empty()) {
    return std::nullopt;
  }
  std::optional<std::string_view> best;
  bool best_affix = false;
  usize best_distance = 0;
  for (const std::string_view candidate : candidates) {
    if (candidate.empty()) {
      continue;
    }
    const bool affix = is_affix_match(input, candidate);
    const usize distance = edit_distance(input, candidate);
    if (!affix) {
      // About a third of the longer name may be wrong; a short name gets
      // one edit. Anything beyond that is a different word, not a typo.
      const usize limit =
          std::max<usize>(1, std::max(input.size(), candidate.size()) / 3);
      if (distance > limit) {
        continue;
      }
    }
    const bool better =
        !best.has_value() || (affix && !best_affix) ||
        (affix == best_affix &&
         (distance < best_distance ||
          (distance == best_distance &&
           (candidate.size() < best->size() ||
            (candidate.size() == best->size() && candidate < *best)))));
    if (better) {
      best = candidate;
      best_affix = affix;
      best_distance = distance;
    }
  }
  return best;
}

namespace {

// A bare token the parser would consume as an option value rather than
// read as a subcommand: `--output check` names no `check` command. The
// tables union every level because the scope is what this scan is
// establishing; over-skipping needs a value-taking flag and a bare token
// spelling a command at once, and then the invocation already failed,
// so the suggestion degrades rather than misleads.
bool takes_value(const std::vector<std::string_view>& longs,
                 const std::vector<char>& shorts,
                 std::string_view token) {
  if (token.starts_with("--")) {
    const usize equal = token.find('=');
    if (equal != std::string_view::npos) {
      return false;
    }
    const std::string_view key = token.substr(2);
    return std::find(longs.begin(), longs.end(), key) != longs.end();
  }
  if (token.size() == 2) {
    return std::find(shorts.begin(), shorts.end(), token[1]) != shorts.end();
  }
  return false;
}

std::optional<std::string_view> selected_subcommand(
    const arg::Command& root,
    std::span<const std::string_view> args) {
  std::vector<std::string_view> subcommands;
  subcommands.reserve(root.subcommands().size());
  for (const arg::Command& sub : root.subcommands()) {
    subcommands.push_back(sub.name());
  }
  std::vector<std::string_view> value_longs;
  std::vector<char> value_shorts;
  const auto collect = [&](const arg::Command& command) {
    for (const arg::Arg& argument : command.args()) {
      if (!argument.is_flag()) {
        if (!argument.long_name().empty()) {
          value_longs.push_back(argument.long_name());
        }
        if (argument.short_name().has_value()) {
          value_shorts.push_back(*argument.short_name());
        }
      }
    }
  };
  collect(root);
  for (const arg::Command& sub : root.subcommands()) {
    collect(sub);
  }
  for (usize i = 1; i < args.size(); ++i) {
    const std::string_view token = args[i];
    if (token.empty()) {
      continue;
    }
    // Past `--` everything is positional, which is also where the parser
    // stops selecting.
    if (token == "--") {
      return std::nullopt;
    }
    // A lone dash is positional for the parser too; only a longer dash
    // token can be a flag with a value to skip.
    if (token.starts_with('-') && token.size() > 1) {
      if (takes_value(value_longs, value_shorts, token)) {
        ++i;
      }
      continue;
    }
    if (std::find(subcommands.begin(), subcommands.end(), token) !=
        subcommands.end()) {
      return token;
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> suggest_subcommand(const arg::Command& root,
                                              std::string_view name) {
  std::vector<std::string_view> candidates;
  candidates.reserve(root.subcommands().size());
  for (const arg::Command& sub : root.subcommands()) {
    candidates.push_back(sub.name());
  }
  if (const auto match = best_match(name, candidates)) {
    return std::string(*match);
  }
  return std::nullopt;
}

std::optional<std::string> suggest_flag(const arg::Command& root,
                                        std::span<const std::string_view> args,
                                        std::string_view key) {
  std::vector<std::string_view> candidates;
  const auto collect = [&](const arg::Command& command) {
    for (const arg::Arg& argument : command.args()) {
      if (!argument.long_name().empty()) {
        candidates.push_back(argument.long_name());
      }
    }
    if (command.builtin_enabled()) {
      candidates.push_back("help");
      candidates.push_back("version");
    }
  };
  collect(root);
  if (const auto selected = selected_subcommand(root, args)) {
    for (const arg::Command& sub : root.subcommands()) {
      if (sub.name() == *selected) {
        collect(sub);
        break;
      }
    }
  }
  if (const auto match = best_match(key, candidates)) {
    return std::string(*match);
  }
  return std::nullopt;
}

}  // namespace cli
