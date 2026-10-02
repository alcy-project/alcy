// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/std_select.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena, i18n::Language::EnUs};

  Fixture() { arena.reserve(1u << 20); }
};

// One manifest dependency, parsed the way a manifest parses it, so the
// cases below exercise the selection layer rather than a retyping of
// its input.
pkg::Dependency dep(Fixture& f, std::string_view spec) {
  std::string text = "[dependencies]\n\"";
  text += spec;
  text += "\" = {}\n";
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(
          "[package]\nname = \"x\"\nversion = \"0.1.0\"\n" + text, "alcy.toml",
          source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(parsed.is_ok());
  if (parsed.is_err()) {
    return pkg::Dependency{};
  }
  pkg::PackageManifest manifest = std::move(parsed).unwrap();
  CHECK(manifest.dependency_count == 1);
  return manifest.dependencies[0];
}

bool has_member(const StdSelection& selection, std::string_view name) {
  for (std::string_view member : selection.members) {
    if (member == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("Selection expands a suite glob to every member") {
  Fixture f;
  const pkg::Dependency entry = dep(f, "alcy/std/*");
  base::Result<StdSelection, diag::Reported> result =
      resolve_std_selection({&entry, 1}, f.bag);
  CHECK(result.is_ok());
  CHECK(!f.bag.has_errors());
  if (result.is_err()) {
    return;
  }
  const StdSelection selection = std::move(result).unwrap();
  CHECK(has_member(selection, "core"));
  CHECK(has_member(selection, "alloc"));
  CHECK(has_member(selection, "fmt"));
  CHECK(has_member(selection, "io"));
}

TEST_CASE("Selection keeps a single member") {
  Fixture f;
  const pkg::Dependency entry = dep(f, "alcy/std/core");
  base::Result<StdSelection, diag::Reported> result =
      resolve_std_selection({&entry, 1}, f.bag);
  CHECK(result.is_ok());
  if (result.is_err()) {
    return;
  }
  const StdSelection selection = std::move(result).unwrap();
  CHECK(selection.members.size() == 1);
  CHECK(has_member(selection, "core"));
}

TEST_CASE("Selection rejects an incomplete closure") {
  Fixture f;
  const pkg::Dependency entry = dep(f, "alcy/std/alloc");
  CHECK(resolve_std_selection({&entry, 1}, f.bag).is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Selection rejects a glob overlapping a member") {
  Fixture f;
  const pkg::Dependency glob = dep(f, "alcy/std/*");
  const pkg::Dependency member = dep(f, "alcy/std/core");
  const pkg::Dependency entries[2] = {glob, member};
  CHECK(resolve_std_selection(entries, f.bag).is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Selection rejects a bare suite and unknown members") {
  struct Case {
    std::string_view spec;
    // What the message has to say, because the spellings are close and
    // the fix each one names is the part a reader needs.
    std::string_view message;
  };
  const Case cases[] = {
      {"alcy/std", "is a suite"},
      {"alcy/nope", "is not a member of alcy/std"},
      {"alcy/std/nope", "is not a member of alcy/std"},
      {"alcy/nope/core", "names no source"},
  };
  for (const Case& c : cases) {
    Fixture f;
    const pkg::Dependency entry = dep(f, c.spec);
    CHECK(resolve_std_selection({&entry, 1}, f.bag).is_err());
    const diag::Diagnostic* const only = f.bag.at(0);
    CHECK(only != nullptr);
    if (only != nullptr) {
      CHECK(only->message.find(c.message) != std::string_view::npos);
    }
  }
}

TEST_CASE("Selection leaves path entries to inclusion") {
  Fixture f;
  std::string bytes =
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\n[dependencies]\n"
      "helper = { path = \"../helper\" }\n";
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                          f.arena);
  CHECK(parsed.is_ok());
  if (parsed.is_err()) {
    return;
  }
  pkg::PackageManifest manifest = std::move(parsed).unwrap();
  base::Result<StdSelection, diag::Reported> result = resolve_std_selection(
      {manifest.dependencies, manifest.dependency_count}, f.bag);
  CHECK(result.is_ok());
  if (result.is_err()) {
    return;
  }
  CHECK(std::move(result).unwrap().members.empty());
}

TEST_CASE("Selection rejects sources it cannot fetch") {
  {
    Fixture f;
    std::string bytes =
        "[package]\nname = \"x\"\nversion = \"0.1.0\"\n[dependencies]\n"
        "\"acme/json\" = { version = \"1\" }\n";
    base::Result<pkg::PackageManifest, diag::Reported> parsed =
        pkg::parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                            f.arena);
    CHECK(parsed.is_ok());
    if (parsed.is_err()) {
      return;
    }
    pkg::PackageManifest manifest = std::move(parsed).unwrap();
    CHECK(resolve_std_selection(
              {manifest.dependencies, manifest.dependency_count}, f.bag)
              .is_err());
  }
  {
    Fixture f;
    std::string bytes =
        "[package]\nname = \"x\"\nversion = \"0.1.0\"\n[dependencies]\n"
        "\"acme/tool\" = { git = \"https://example.com/t.git\" }\n";
    base::Result<pkg::PackageManifest, diag::Reported> parsed =
        pkg::parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                            f.arena);
    CHECK(parsed.is_ok());
    if (parsed.is_err()) {
      return;
    }
    pkg::PackageManifest manifest = std::move(parsed).unwrap();
    CHECK(resolve_std_selection(
              {manifest.dependencies, manifest.dependency_count}, f.bag)
              .is_err());
  }
}

}  // namespace pipeline
