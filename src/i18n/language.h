// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "debug/fatal.h"
#include "fpag/base/numeric.h"

namespace i18n {

// The languages the compiler reports in. Every message has a text in
// each of them, so an entry here is a promise the catalog keeps: a
// language without a catalog is not a language the cli accepts.
enum class Language : u8 {
  EnUs,
  Count,
};

// A language and the tag that names it. The tag is what `--lang`
// takes, what a catalog is keyed by, and what a bug report spells, so
// one language has exactly one spelling. Lowercase kebab-case, which
// is not what BCP 47 writes: the flag is read and typed by people, and
// a tag read off the help text has to work as written.
struct LanguageTag {
  Language language;
  std::string_view tag;
};

inline constexpr std::array LANGUAGE_TAGS = {
    LanguageTag{Language::EnUs, "en-us"},
};

inline constexpr usize LANGUAGE_COUNT = static_cast<usize>(Language::Count);

static_assert(LANGUAGE_TAGS.size() == LANGUAGE_COUNT,
              "every Language needs exactly one tag");

// The tag a language is named by, spelled the way `--lang` spells it.
constexpr std::string_view canonical_tag(Language language) {
  for (const LanguageTag& entry : LANGUAGE_TAGS) {
    if (entry.language == language) {
      return entry.tag;
    }
  }
  UNREACHABLE();
}

// A language named by its tag, or nothing when the tag names no
// catalog. Matching is exact: the help text documents one spelling per
// language, and a value that differs from it is a mistake to report
// rather than a spelling to guess at. Nothing here reads the
// environment - the locale a shell exports is the interface of another
// program, and a build whose output language depends on it is a
// different compiler on every machine.
constexpr std::optional<Language> language_from_tag(std::string_view tag) {
  for (const LanguageTag& entry : LANGUAGE_TAGS) {
    if (entry.tag == tag) {
      return entry.language;
    }
  }
  return std::nullopt;
}

// Every tag, space separated, for a message that has to name what it
// accepts.
std::string tag_list();

}  // namespace i18n
