// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fpag/base/result.h"

namespace ssg {

// The site's catalog: one authored JSON file, shared by the client
// modules (compiled straight into the pages) and by the generator,
// which stamps labels at build time. The generator reads the JSON as
// it is at run time; there is no second copy of the table. The
// layout is fixed:
//
//   {
//     "languages": ["en", "ja"],
//     "default": "en",
//     "names": { "en": "English", "ja": "<japanese name>" },
//     "messages": { "en": { ... }, "ja": { ... } }
//   }
//
// Every key the default language carries is translated; a key missing
// in another language falls back to the default, then to the key, the
// same rule the compiled pages follow. A key a non-default language
// names but the default does not have fails the build: it is almost
// certainly a typo.

struct Catalog {
  std::vector<std::string> languages;
  std::string default_language;
  std::unordered_map<std::string, std::string> language_names;
  // The default language's message table. Unordered: lookups are by
  // key, and the catalog file's own order carries no meaning.
  std::unordered_map<std::string, std::string> messages;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      per_language;
};

struct LanguageInfo {
  std::string name;
  std::string prefix;  // "" for the default language, "ja/" otherwise
};

base::Result<Catalog, std::string> load_catalog(const std::string& path);

std::string translate(const Catalog& catalog,
                      std::string_view language,
                      std::string_view key);
std::string translate(
    const Catalog& catalog,
    std::string_view language,
    std::string_view key,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        params);

// The URL side of the site's languages: every page exists once per
// language; the default language is the tree at the site root and
// every other language lives under its own prefix (`/ja/guide/...`).
std::string prefix_of(const Catalog& catalog, std::string_view language);
std::vector<std::string> other_languages(const Catalog& catalog,
                                         std::string_view language);

LanguageInfo language_info_of(const Catalog& catalog,
                              std::string_view language);

}  // namespace ssg
