// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "doctest/doctest.h"
#include "i18n/language.h"
#include "i18n/messages.h"

namespace i18n {

namespace {

TEST_CASE("a tag names exactly one language") {
  CHECK(canonical_tag(Language::EnUs) == "en-us");
  const std::optional<Language> found = language_from_tag("en-us");
  CHECK(found.has_value());
  CHECK(found.value() == Language::EnUs);
}

TEST_CASE("a tag is matched as written") {
  // The help text documents one spelling per language, so a value that
  // differs from it is a mistake to report rather than one to guess at.
  CHECK(!language_from_tag("EN-US").has_value());
  CHECK(!language_from_tag("en_US").has_value());
  CHECK(!language_from_tag("en").has_value());
  CHECK(!language_from_tag("").has_value());
  CHECK(!language_from_tag("ja-jp").has_value());
}

TEST_CASE("the tag list names every language") {
  CHECK(tag_list() == "en-us");
}

TEST_CASE("every catalog has one entry per key, in key order") {
  static_assert(detail::is_canonical(detail::EnUs::entries));
  CHECK(detail::EnUs::entries.size() == KEY_COUNT);
  for (u16 i = 0; i < KEY_COUNT; ++i) {
    CHECK(detail::EnUs::entries[i].key == static_cast<Key>(i));
  }
}

TEST_CASE("a message with no placeholders renders as itself") {
  CHECK(text<Key::LexerInvalidCharacter>(Language::EnUs) ==
        "invalid character");
  CHECK(format<Key::LexerInvalidNumber>(Language::EnUs) ==
        "invalid number literal");
}

}  // namespace

}  // namespace i18n
