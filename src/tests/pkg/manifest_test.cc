// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/manifest.h"

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

}  // namespace pkg
