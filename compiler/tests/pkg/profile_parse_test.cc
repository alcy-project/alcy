// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "pkg/profile.h"
#include "source/source.h"

namespace pkg {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena, i18n::Language::EnUs};

  Fixture() { arena.reserve(1u << 20); }
};

base::Result<Profiles, diag::Reported> parse(Fixture& f,
                                             std::string_view bytes) {
  return parse_profile(bytes, ".alcy/profile.toml", source::UNKNOWN_FILE, f.bag,
                       f.arena);
}

}  // namespace

TEST_CASE("Profile parses an optimizing table") {
  Fixture f;
  base::Result<Profiles, diag::Reported> result =
      parse(f, "[fast]\noptimize = true\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const Profiles profiles = std::move(result).unwrap();
  CHECK(profiles.tables.size() == 1);
  if (profiles.tables.size() != 1) {
    return;
  }
  CHECK(profiles.tables[0].name == "fast");
  CHECK(profiles.tables[0].settings.optimize == true);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile without a file means no tables") {
  Fixture f;
  base::Result<Profiles, diag::Reported> empty = parse(f, "");
  CHECK(empty.is_ok());
  if (!empty.is_ok()) {
    return;
  }
  CHECK(std::move(empty).unwrap().tables.empty());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile without a key leaves it unset") {
  Fixture f;
  base::Result<Profiles, diag::Reported> result = parse(f, "[debug]\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const Profiles profiles = std::move(result).unwrap();
  CHECK(profiles.tables.size() == 1);
  if (profiles.tables.size() != 1) {
    return;
  }
  CHECK(!profiles.tables[0].settings.optimize.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile ignores unknown keys") {
  Fixture f;
  base::Result<Profiles, diag::Reported> result =
      parse(f, "[release]\noptimize = true\nfrobnicator = 3\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const Profiles profiles = std::move(result).unwrap();
  CHECK(profiles.tables.size() == 1);
  if (profiles.tables.size() != 1) {
    return;
  }
  CHECK(profiles.tables[0].name == "release");
  CHECK(profiles.tables[0].settings.optimize == true);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile keeps table order") {
  Fixture f;
  base::Result<Profiles, diag::Reported> result =
      parse(f, "[first]\noptimize = false\n[second]\noptimize = true\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const Profiles profiles = std::move(result).unwrap();
  CHECK(profiles.tables.size() == 2);
  if (profiles.tables.size() != 2) {
    return;
  }
  CHECK(profiles.tables[0].name == "first");
  CHECK(profiles.tables[0].settings.optimize == false);
  CHECK(profiles.tables[1].name == "second");
  CHECK(profiles.tables[1].settings.optimize == true);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile rejects a mistyped optimize") {
  Fixture f;
  CHECK(parse(f, "[fast]\noptimize = \"yes\"\n").is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Profile parses an on-panic behavior") {
  Fixture f;
  base::Result<Profiles, diag::Reported> result =
      parse(f, "[debug]\non-panic = \"backtrace\"\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const Profiles profiles = std::move(result).unwrap();
  CHECK(profiles.tables.size() == 1);
  if (profiles.tables.size() != 1) {
    return;
  }
  CHECK(profiles.tables[0].name == "debug");
  CHECK(profiles.tables[0].settings.on_panic == OnPanic::Backtrace);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile leaves on-panic unset when unnamed") {
  Fixture f;
  base::Result<Profiles, diag::Reported> result = parse(f, "[fast]\n");
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const Profiles profiles = std::move(result).unwrap();
  CHECK(profiles.tables.size() == 1);
  if (profiles.tables.size() != 1) {
    return;
  }
  CHECK(!profiles.tables[0].settings.on_panic.has_value());
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile rejects an unknown on-panic behavior") {
  Fixture f;
  CHECK(parse(f, "[debug]\non-panic = \"unwind\"\n").is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Profile rejects a mistyped on-panic behavior") {
  Fixture f;
  CHECK(parse(f, "[debug]\non-panic = true\n").is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Profile rejects a key that is not a table") {
  Fixture f;
  CHECK(parse(f, "optimize = true\n").is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Profile rejects broken TOML") {
  Fixture f;
  CHECK(parse(f, "[unclosed\n").is_err());
  CHECK(f.bag.has_errors());
}

}  // namespace pkg
