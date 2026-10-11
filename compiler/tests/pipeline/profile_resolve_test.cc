// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <span>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "pipeline/profile.h"
#include "pkg/profile.h"

namespace pipeline {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena, i18n::Language::EnUs};

  Fixture() { arena.reserve(1u << 20); }
};

// Whether the bag holds a diagnostic with exactly this text.
bool has_message(const diag::DiagBag& bag, std::string_view message) {
  for (u32 i = 0; i < bag.size(); ++i) {
    const diag::Diagnostic* const diagnostic = bag.at(i);
    if (diagnostic != nullptr && diagnostic->message == message) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("Profile defaults to the debug built-in") {
  Fixture f;
  base::Result<BuildProfile, diag::Reported> result =
      resolve_profile(ProfileRequest{}, pkg::Profiles{}, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(!std::move(result).unwrap().optimize);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile resolves the release built-in") {
  Fixture f;
  base::Result<BuildProfile, diag::Reported> result = resolve_profile(
      ProfileRequest{.name = "release"}, pkg::Profiles{}, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(std::move(result).unwrap().optimize);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile applies the keys a declared table lists") {
  Fixture f;
  const pkg::ProfileTable table{
      .name = "fast", .settings = pkg::ProfileSettings{.optimize = true}};
  const std::span<const pkg::ProfileTable> tables(&table, 1);
  base::Result<BuildProfile, diag::Reported> result = resolve_profile(
      ProfileRequest{.name = "fast"}, pkg::Profiles{.tables = tables}, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(std::move(result).unwrap().optimize);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile lets a table override a built-in key") {
  Fixture f;
  const pkg::ProfileTable table{
      .name = "debug", .settings = pkg::ProfileSettings{.optimize = true}};
  const std::span<const pkg::ProfileTable> tables(&table, 1);
  base::Result<BuildProfile, diag::Reported> result = resolve_profile(
      ProfileRequest{.name = "debug"}, pkg::Profiles{.tables = tables}, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(std::move(result).unwrap().optimize);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile keeps a built-in key a table leaves unset") {
  Fixture f;
  const pkg::ProfileTable table{.name = "release", .settings = {}};
  const std::span<const pkg::ProfileTable> tables(&table, 1);
  base::Result<BuildProfile, diag::Reported> result =
      resolve_profile(ProfileRequest{.name = "release"},
                      pkg::Profiles{.tables = tables}, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(std::move(result).unwrap().optimize);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile does not inherit the release keys") {
  Fixture f;
  const pkg::ProfileTable table{.name = "fast", .settings = {}};
  const std::span<const pkg::ProfileTable> tables(&table, 1);
  base::Result<BuildProfile, diag::Reported> result = resolve_profile(
      ProfileRequest{.name = "fast"}, pkg::Profiles{.tables = tables}, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(!std::move(result).unwrap().optimize);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Profile refuses a name nothing declares") {
  Fixture f;
  CHECK(resolve_profile(ProfileRequest{.name = "nope"}, pkg::Profiles{}, f.bag)
            .is_err());
  CHECK(has_message(f.bag,
                    "Unknown profile 'nope'; the built-in profiles are debug "
                    "and release, and .alcy/profile.toml defines the rest"));
}

}  // namespace pipeline
