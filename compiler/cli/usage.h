// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli/suggest.h"
#include "fpag/arg/arg.h"
#include "fpag/arg/command.h"
#include "fpag/arg/parse_error.h"
#include "fpag/base/numeric.h"
#include "fpag/term/color_style.h"
#include "i18n/language.h"
#include "i18n/messages.h"

namespace cli {

// Every string a parser points at, one table per language.
//
// fpag's Arg holds its help text as a view, so the text has to outlive
// every parser built from it. A table is filled on first use and lives
// as long as the process, which is what lets a test build a second
// parser over text the first one already used. Only the messages the
// cli renders are in it: a key the compiler reports through has no
// business here, and asking for one yields an empty view.
class UsageText {
 public:
  explicit UsageText(i18n::Language language);

  // The text of one message, rendered for this language. Empty for a key
  // no one filled, which is every message the compiler reports through.
  std::string_view text(i18n::Key key) const {
    return texts_[static_cast<usize>(key)];
  }

  // Renders one message into this table. Only the messages the cli
  // renders belong here.
  void set(i18n::Key key, std::string value) {
    texts_[static_cast<usize>(key)] = std::move(value);
  }

 private:
  std::vector<std::string> texts_;
};

// The table for one language, built on first use.
const UsageText& usage_text(i18n::Language language);

// The help as the invocation's language writes it, in the layout fpag
// lays out: the option column, the description column, and the labels
// between them. The labels and the descriptions come from the catalog;
// the columns, the brackets, and the flag spellings are structure, and
// no language reorders them.
struct HelpFormatter {
  i18n::Language language = i18n::Language::EnUs;

  std::string operator()(const arg::Command& command,
                         term::ColorStyle color_style) const;
};

// One line per parse error, then where to read more. The error itself
// is a message, so it is composed by the catalog against the flag and
// the value the parser rejected; the label in front of it and the
// punctuation after it are structure. A suggestion follows the error it
// explains, naming the flag without dashes; rendering adds them.
struct ErrorFormatter {
  i18n::Language language = i18n::Language::EnUs;

  std::string operator()(std::string_view command_name,
                         const std::vector<arg::ParseError>& errors,
                         std::span<const FlagSuggestion> suggestions,
                         term::ColorStyle color_style) const;
};

}  // namespace cli
