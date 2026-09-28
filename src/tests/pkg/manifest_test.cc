// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/manifest.h"

#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace pkg {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena};

  Fixture() { arena.reserve(1u << 20); }
};

constexpr std::string_view VALID_MANIFEST =
    "[package]\n"
    "name = \"hello\"\n"
    "version = \"0.1.0\"\n"
    "edition = \"2026\"\n"
    "\n"
    "[dependencies]\n"
    "foo = { path = \"../foo\" }\n"
    "bar = { path = \"libs/bar\" }\n";

}  // namespace

TEST_CASE("Version parses strict X.Y.Z") {
  CHECK(parse_version("0.1.0").unwrap() == Version{0, 1, 0});
  CHECK(parse_version("12.34.56").unwrap() == Version{12, 34, 56});
  CHECK(parse_version("").unwrap_err() == VersionError::Empty);
  CHECK(parse_version("1").unwrap_err() == VersionError::BadFormat);
  CHECK(parse_version("1.2").unwrap_err() == VersionError::BadFormat);
  CHECK(parse_version("1.2.3.4").unwrap_err() == VersionError::BadFormat);
  CHECK(parse_version("1.2.").unwrap_err() == VersionError::BadFormat);
  CHECK(parse_version("a.b.c").unwrap_err() == VersionError::BadFormat);
  CHECK(parse_version("1.2.x").unwrap_err() == VersionError::BadFormat);
  CHECK(parse_version("1.0.0-alpha").unwrap_err() == VersionError::BadFormat);
}

TEST_CASE("Manifest parses a valid package") {
  Fixture f;
  base::Result<PackageManifest, diag::Reported> result = parse_manifest(
      VALID_MANIFEST, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const PackageManifest manifest = std::move(result).unwrap();
  CHECK(manifest.name == "hello");
  CHECK(manifest.version == Version{0, 1, 0});
  CHECK(manifest.edition == "2026");
  CHECK(!f.bag.has_errors());
  CHECK(manifest.dependency_count == 2);
  if (manifest.dependency_count != 2) {
    return;
  }
  // toml++ tables iterate key-sorted, so dependency order is deterministic
  // but not manifest order.
  CHECK(manifest.dependencies[0].name == "bar");
  CHECK(manifest.dependencies[0].path == "libs/bar");
  CHECK(manifest.dependencies[1].name == "foo");
  CHECK(manifest.dependencies[1].path == "../foo");
}

TEST_CASE("Manifest without dependencies parses") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"solo\"\nversion = \"2.0.0\"\n";
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const PackageManifest manifest = std::move(result).unwrap();
  CHECK(manifest.name == "solo");
  CHECK(manifest.dependency_count == 0);
  CHECK(manifest.dependencies == nullptr);
  CHECK(manifest.edition.empty());
  CHECK(manifest.bin_count == 0);
  CHECK(manifest.bins == nullptr);
}

TEST_CASE("Manifest parses binary targets") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
      "\n"
      "[[bin]]\npath = \"main.al\"\n"
      "\n"
      "[[bin]]\nname = \"tool\"\npath = \"tool.al\"\n";
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const PackageManifest manifest = std::move(result).unwrap();
  CHECK(!f.bag.has_errors());
  CHECK(manifest.bin_count == 2);
  if (manifest.bin_count != 2) {
    return;
  }
  CHECK(manifest.bins[0].name.empty());
  CHECK(manifest.bins[0].path == "main.al");
  CHECK(manifest.bins[1].name == "tool");
  CHECK(manifest.bins[1].path == "tool.al");
}

TEST_CASE("Manifest rejects binary targets without paths") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
      "\n"
      "[[bin]]\nname = \"tool\"\n";
  CHECK(parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena)
            .is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Manifest syntax errors carry spans") {
  Fixture f;
  constexpr std::string_view bytes = "[package]\nname = \n";
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(bytes, "alcy.toml", 3, f.bag, f.arena);
  CHECK(result.is_err());
  CHECK(f.bag.has_errors());
  CHECK(f.bag.size() == 1);
  if (f.bag.size() != 1) {
    return;
  }
  const diag::Diagnostic* const diag = f.bag.at(0);
  CHECK(diag != nullptr);
  if (diag == nullptr) {
    return;
  }
  CHECK(diag->severity == diag::Severity::Error);
  CHECK(diag->code == 1000);
  CHECK(diag->has_primary_span);
  CHECK(diag->primary_span.file == 3);
}

TEST_CASE("Manifest semantic errors are diagnosed") {
  Fixture f;
  constexpr std::string_view no_package = "[other]\n";
  CHECK(parse_manifest(no_package, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                       f.arena)
            .is_err());
  constexpr std::string_view no_version = "[package]\nname = \"x\"\n";
  CHECK(parse_manifest(no_version, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                       f.arena)
            .is_err());
  constexpr std::string_view bad_version =
      "[package]\nname = \"x\"\nversion = \"nope\"\n";
  CHECK(parse_version("nope").is_err());
  CHECK(parse_manifest(bad_version, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                       f.arena)
            .is_err());
  constexpr std::string_view registry_dep =
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\n"
      "[dependencies]\nfoo = \"1.0\"\n";
  CHECK(parse_manifest(registry_dep, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                       f.arena)
            .is_err());
  CHECK(f.bag.error_count() == 4);
}

namespace {

PackageManifest parse_ok(std::string_view bytes, Fixture& f) {
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  CHECK(!f.bag.has_errors());
  if (result.is_err()) {
    return PackageManifest{};
  }
  return std::move(result).unwrap();
}

constexpr std::string_view DEPS_HEAD =
    "[package]\nname = \"x\"\nversion = \"0.1.0\"\n[dependencies]\n";

}  // namespace

TEST_CASE("Manifest parses suite and package specifiers") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\n[dependencies]\n"
      "\"alcy/std/*\" = {}\n"
      "\"alcy/std/core\" = {}\n";
  const PackageManifest manifest = parse_ok(bytes, f);
  CHECK(manifest.dependency_count == 2);
  if (manifest.dependency_count != 2) {
    return;
  }
  // toml++ tables iterate key-sorted, so the glob (`*` sorts before
  // letters) comes first.
  const Dependency& glob = manifest.dependencies[0];
  CHECK(glob.spec == "alcy/std/*");
  CHECK(glob.owner == "alcy");
  CHECK(glob.suite == "std");
  CHECK(glob.suite_glob);
  CHECK(glob.member.empty());
  CHECK(glob.source == DependencySource::Unspecified);
  const Dependency& member = manifest.dependencies[1];
  CHECK(member.spec == "alcy/std/core");
  CHECK(member.member == "core");
  CHECK(!member.suite_glob);
}

TEST_CASE("Manifest parses registry and git sources") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\n[dependencies]\n"
      "\"acme/json\" = { version = \"1\" }\n"
      "\"acme/exact\" = { version = \"=2.3.4\" }\n"
      "\"acme/tool\" = { git = \"https://example.com/t.git\", "
      "path = \"pkg/tool\" }\n"
      "\"acme/pinned\" = { git = \"https://example.com/p.git\", "
      "tag = \"v0.9.0\" }\n"
      "\"acme/local\" = { path = \"../local\" }\n";
  const PackageManifest manifest = parse_ok(bytes, f);
  CHECK(manifest.dependency_count == 5);
  if (manifest.dependency_count != 5) {
    return;
  }
  // Key-sorted: exact, json, local, pinned, tool.
  CHECK(manifest.dependencies[0].source == DependencySource::Registry);
  CHECK(manifest.dependencies[0].version == "=2.3.4");
  CHECK(manifest.dependencies[1].version == "1");
  CHECK(manifest.dependencies[1].registry.empty());
  CHECK(manifest.dependencies[2].source == DependencySource::Path);
  CHECK(manifest.dependencies[2].path == "../local");
  CHECK(manifest.dependencies[3].source == DependencySource::Git);
  CHECK(manifest.dependencies[3].git_ref_kind == "tag");
  CHECK(manifest.dependencies[3].git_ref == "v0.9.0");
  CHECK(manifest.dependencies[4].source == DependencySource::Git);
  CHECK(manifest.dependencies[4].path == "pkg/tool");
  CHECK(manifest.dependencies[4].git_ref_kind.empty());
}

TEST_CASE("Manifest parses --deps fragments like manifest entries") {
  Fixture f;
  base::Result<Dependency, diag::Reported> bare =
      parse_dependency_flag(f.bag, f.arena, "alcy/std/*");
  CHECK(bare.is_ok());
  if (bare.is_ok()) {
    CHECK(std::move(bare).unwrap().suite_glob);
  }
  base::Result<Dependency, diag::Reported> valued =
      parse_dependency_flag(f.bag, f.arena, "acme/json = { version = \"1\" }");
  CHECK(valued.is_ok());
  if (valued.is_ok()) {
    const Dependency dep = std::move(valued).unwrap();
    CHECK(dep.owner == "acme");
    CHECK(dep.source == DependencySource::Registry);
    CHECK(dep.version == "1");
  }
  CHECK(parse_dependency_flag(f.bag, f.arena, "acme/x = { frobnicate = 1 }")
            .is_err());
  CHECK(parse_dependency_flag(f.bag, f.arena, "acme/x = ").is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Manifest rejects malformed specifiers and sources") {
  struct Case {
    std::string_view entry;
  };
  const Case cases[] = {
      {"\"a/b/c/d\" = {}\n"},
      {"\"a//b\" = {}\n"},
      {"\"a/*/b\" = {}\n"},
      {"\"*\" = {}\n"},
      {"\"acme/x\" = { version = \"1\", git = \"https://e.com/r.git\" }\n"},
      {"\"acme/x\" = { version = \"1\", path = \"../x\" }\n"},
      {"\"acme/x\" = { version = \"^1\" }\n"},
      {"\"acme/x\" = { version = \"1.2.3.4\" }\n"},
      {"\"acme/x\" = { branch = \"main\" }\n"},
      {"\"acme/x\" = { git = \"https://e.com/r.git\", branch = \"a\", "
       "tag = \"b\" }\n"},
      {"\"acme/x\" = { frobnicate = \"yes\" }\n"},
      {"\"acme/x\" = \"1.0\"\n"},
  };
  for (const Case& c : cases) {
    Fixture f;
    std::string bytes(DEPS_HEAD);
    bytes += c.entry;
    CHECK(
        parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena)
            .is_err());
  }
}

}  // namespace pkg
