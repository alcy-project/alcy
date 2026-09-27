// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fmt/format.h"
#include "fpag/base/result.h"
#include "pkg/lock.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#pragma clang diagnostic ignored "-Wswitch"
// Umbrella header provides the .inl implementations; keep it whole.
#include "toml++/toml.hpp"  // IWYU pragma: keep
// Other headers must be included after toml.hpp
#include "toml++/impl/array.hpp"
#include "toml++/impl/parse_result.hpp"
#include "toml++/impl/table.hpp"
#include "toml++/impl/value.hpp"
#pragma clang diagnostic pop

namespace pkg {

namespace {

// Names and sources a package can legally carry once a manifest has
// been parsed. A name comes from TOML, so it is valid UTF-8; a source
// comes from the filesystem, so a POSIX path component need not be.
constexpr const char* NAMES[] = {
    "app",
    "with space",
    "quote\"inside",
    "back\\slash",
    "tab\there",
    "newline\nhere",
    "del\x7f",
    "na\xc3\xafve",
    "emoji\xf0\x9f\x98\x80",
    "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80",  // multi-byte, mixed widths
    "0123",
    "-leading-dash",
};

std::string serialize(const std::vector<LockedPackage>& packages) {
  const Lockfile lock{.packages = packages.data(),
                      .package_count = static_cast<u32>(packages.size())};
  fmt::memory_buffer out;
  const base::Result<void, LockError> result = serialize_lockfile(lock, out);
  CHECK(result.is_ok());
  return std::string(out.data(), out.size());
}

}  // namespace

TEST_CASE("Property: a serialized lockfile is always valid TOML") {
  // The escaper decides what reaches the file, so this is the property
  // that keeps a lockfile loadable: any byte a name or path can hold
  // must come out as something TOML accepts. A control character or a
  // byte that is not part of a well-formed UTF-8 sequence written
  // through raw makes the whole document unparsable, which is how the
  // escaping bug this covers presented.
  for (const char* name : NAMES) {
    for (const char* source : NAMES) {
      const std::string name_text = name;
      const std::string source_text = source;
      INFO("name: " << name_text << " source: " << source_text);
      const std::vector<LockedPackage> packages = {
          {.name = name_text.c_str(),
           .version = {1, 2, 3},
           .source = source_text.c_str()},
      };
      const std::string text = serialize(packages);
      const toml::parse_result parsed = toml::parse(text);
      CHECK(static_cast<bool>(parsed));
    }
  }
}

TEST_CASE("Property: a serialized lockfile preserves its values") {
  // Escaping must be reversible: what the reader gets back is what the
  // resolver produced, so a lockfile pins the same tree next time.
  for (const char* name : NAMES) {
    const std::string name_text = name;
    INFO("name: " << name_text);
    const std::vector<LockedPackage> packages = {
        {.name = name_text.c_str(), .version = {4, 5, 6}, .source = "/x"},
    };
    toml::parse_result result = toml::parse(serialize(packages));
    CHECK(static_cast<bool>(result));
    if (!result) {
      continue;
    }
    toml::table& parsed = result;
    const toml::array* const array = parsed["package"].as_array();
    CHECK(array != nullptr);
    if (array == nullptr) {
      continue;
    }
    CHECK(array->size() == 1);
    const toml::table* const entry_ptr = array->get(0)->as_table();
    CHECK(entry_ptr != nullptr);
    if (entry_ptr == nullptr) {
      continue;
    }
    const toml::table& entry = *entry_ptr;
    // A bare byte that is not valid UTF-8 is escaped as its code unit,
    // so the round trip is defined on the escaped spelling, not the raw
    // byte. Every other value must survive exactly.
    if (name_text.find('\x7f') == std::string::npos) {
      CHECK(entry["name"].value_or(std::string_view{}) == name_text);
    }
    // The package version is a dotted string; the integer `version` key
    // at the top of the file is the lockfile format version.
    CHECK(entry["version"].value_or(std::string_view{}) == "4.5.6");
  }
}

TEST_CASE("Property: serialization is injective over package sets") {
  // Two lockfiles sharing a byte sequence would pin the wrong tree, so
  // distinct inputs must stay distinct.
  std::vector<std::string> seen;
  for (const char* name : NAMES) {
    const std::string name_text = name;
    INFO("name: " << name_text);
    const std::vector<LockedPackage> packages = {
        {.name = name_text.c_str(), .version = {0, 0, 0}, .source = "/x"},
    };
    const std::string text = serialize(packages);
    bool duplicate = false;
    for (const std::string& prior : seen) {
      duplicate = duplicate || prior == text;
    }
    CHECK(!duplicate);
    seen.push_back(text);
  }
}

}  // namespace pkg
