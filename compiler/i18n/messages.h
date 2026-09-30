// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "debug/fatal.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "i18n/language.h"
#include "i18n/messages.def"

namespace i18n {

// One identity per message in the catalog, named by the message and not
// by the code it is reported under. The ordinal is an index into every
// catalog, so nothing outside this module depends on which one a
// message got. Sixteen bits because the catalog outgrows a byte: every
// message the compiler prints and every message the cli renders is one
// ordinal, and a catalog of a few hundred entries is the design.
// NOLINTNEXTLINE(performance-enum-size)
enum class Key : u16 {
#define ALCY_I18N_ENUMERATOR(Name, Text) Name,
  ALCY_I18N_FOREACH_KEY_EN_US(ALCY_I18N_ENUMERATOR)
#undef ALCY_I18N_ENUMERATOR
      Count,
};

inline constexpr u16 KEY_COUNT = static_cast<u16>(Key::Count);

namespace detail {

// A message and the text it has in one language.
struct Entry {
  Key key;
  std::string_view text;
};

#define ALCY_I18N_TEXT_CASE(Name, Text) \
  case Key::Name: return Text;
#define ALCY_I18N_ENTRY(Name, Text) Entry{Key::Name, Text},

// One catalog per language, generated from that language's list: the
// text of one message as a compile-time constant, and the whole
// language as a table the build can check.
#define ALCY_I18N_CATALOG(LanguageValue, Catalog, LIST)       \
  struct Catalog {                                            \
    template <Key K>                                          \
    static consteval std::string_view text() {                \
      switch (K) {                                            \
        LIST(ALCY_I18N_TEXT_CASE)                             \
        default: UNREACHABLE();                               \
      }                                                       \
    }                                                         \
    static consteval std::array<Entry, KEY_COUNT> entries() { \
      return {LIST(ALCY_I18N_ENTRY)};                         \
    }                                                         \
  };

// The languages the catalogs cover, in enum order. The runtime switch,
// the tag table, and the cli's choice list all walk this one list.
#define ALCY_I18N_FOREACH_LANGUAGE(F) \
  F(Language::EnUs, EnUs, ALCY_I18N_FOREACH_KEY_EN_US)

ALCY_I18N_FOREACH_LANGUAGE(ALCY_I18N_CATALOG)

// A catalog that is not one entry per key, in key order, hands a
// caller another message's text, which is a wrong message rather than
// a missing one.
template <usize N>
consteval bool is_canonical(const std::array<Entry, N>& entries) {
  for (usize i = 0; i < N; ++i) {
    if (entries[i].key != static_cast<Key>(i)) {
      return false;
    }
  }
  return true;
}

static_assert(is_canonical(EnUs::entries()));

// A message rendered without arguments has nothing to fill in: the
// empty-argument format string parses the text and rejects a
// placeholder that no argument can satisfy.
consteval void require_no_arguments(std::string_view text) {
  const fmt::format_string<> format(text);
  (void)format;
}

}  // namespace detail

// The text of a message that takes no arguments, in one language. A
// message with a placeholder does not compile here: the caller would
// print the placeholder.
template <Key K>
std::string_view text(Language language) {
  switch (language) {
#define ALCY_I18N_TEXT_IN(LanguageValue, Catalog, LIST)       \
  case LanguageValue:                                         \
    detail::require_no_arguments(detail::Catalog::text<K>()); \
    return detail::Catalog::text<K>();
    ALCY_I18N_FOREACH_LANGUAGE(ALCY_I18N_TEXT_IN)
#undef ALCY_I18N_TEXT_IN
    default: UNREACHABLE();
  }
}

// Composes a message in one language into out, which is anything fmt can
// format into. Every catalog's format string for K is parsed against
// Args... at compile time, so a translation that lost a placeholder or
// retyped one is a build error rather than a diagnostic printed with a
// hole in it.
template <Key K, typename OutputIt, typename... Args>
void format_to(OutputIt&& out, Language language, Args&&... args) {
  switch (language) {
#define ALCY_I18N_FORMAT_IN(LanguageValue, Catalog, LIST)                   \
  case LanguageValue:                                                       \
    fmt::format_to(out,                                                     \
                   fmt::format_string<Args...>(detail::Catalog::text<K>()), \
                   std::forward<Args>(args)...);                            \
    return;
    ALCY_I18N_FOREACH_LANGUAGE(ALCY_I18N_FORMAT_IN)
#undef ALCY_I18N_FORMAT_IN
    default: UNREACHABLE();
  }
}

// The same message as a string, for a caller that renders it instead of
// appending it: the cli report, and the text a parser is built with.
template <Key K, typename... Args>
std::string format(Language language, Args&&... args) {
  fmt::memory_buffer out;
  format_to<K>(std::back_inserter(out), language, std::forward<Args>(args)...);
  return std::string(out.data(), out.size());
}

#undef ALCY_I18N_FOREACH_LANGUAGE
#undef ALCY_I18N_CATALOG
#undef ALCY_I18N_ENTRY
#undef ALCY_I18N_TEXT_CASE

}  // namespace i18n
