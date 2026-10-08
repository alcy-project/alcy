// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pkg/manifest.h"

#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "pkg/version.h"
#include "pkg/version_req.h"
#include "source/source.h"

namespace pkg {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena, i18n::Language::EnUs};

  Fixture() { arena.reserve(1u << 20); }
};

constexpr std::string_view VALID_MANIFEST =
    "[package]\n"
    "name = \"hello\"\n"
    "version = \"0.1.0\"\n"
    "license = \"\"\n"
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
      "[package]\nname = \"solo\"\nversion = \"2.0.0\"\nlicense = \"\"\n";
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
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

TEST_CASE("Manifest parses a library target") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"hash\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
      "\n"
      "[lib]\nname = \"hash\"\npath = \"lib.al\"\n";
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const PackageManifest manifest = std::move(result).unwrap();
  CHECK(!f.bag.has_errors());
  CHECK(manifest.bin_count == 0);
  CHECK(manifest.lib != nullptr);
  if (manifest.lib == nullptr) {
    return;
  }
  CHECK(manifest.lib->name == "hash");
  CHECK(manifest.lib->path == "lib.al");
  CHECK(verify_manifest(manifest).is_ok());
}

TEST_CASE("Manifest library names default empty") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"hash\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
      "\n"
      "[lib]\npath = \"lib.al\"\n";
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const PackageManifest manifest = std::move(result).unwrap();
  CHECK(!f.bag.has_errors());
  CHECK(manifest.lib != nullptr);
  if (manifest.lib == nullptr) {
    return;
  }
  CHECK(manifest.lib->name.empty());
  CHECK(manifest.lib->path == "lib.al");
  CHECK(verify_manifest(manifest).is_ok());
}

TEST_CASE("Manifest rejects library targets without paths") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"hash\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
      "\n"
      "[lib]\nname = \"hash\"\n";
  CHECK(parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena)
            .is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Manifest parses the [spec] suite-only list") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"core\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
      "\n"
      "[spec]\nsuite-only = [\"Index\", \"Eq\"]\n";
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const PackageManifest manifest = std::move(result).unwrap();
  CHECK(!f.bag.has_errors());
  CHECK(manifest.suite_only_count == 2);
  if (manifest.suite_only_count != 2) {
    return;
  }
  CHECK(manifest.suite_only[0] == "Index");
  CHECK(manifest.suite_only[1] == "Eq");
  CHECK(verify_manifest(manifest).is_ok());
}

TEST_CASE("Manifest without a [spec] table seals nothing") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"solo\"\nversion = \"2.0.0\"\nlicense = \"\"\n";
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (result.is_ok()) {
    const PackageManifest manifest = std::move(result).unwrap();
    CHECK(manifest.suite_only == nullptr);
    CHECK(manifest.suite_only_count == 0);
  }
  // An empty table and an empty list are both the same as none at all.
  constexpr std::string_view empty_table =
      "[package]\nname = \"solo\"\nversion = \"2.0.0\"\nlicense = "
      "\"\"\n[spec]\n";
  constexpr std::string_view empty_list =
      "[package]\nname = \"solo\"\nversion = \"2.0.0\"\nlicense = \"\"\n"
      "[spec]\nsuite-only = []\n";
  for (const std::string_view table : {empty_table, empty_list}) {
    base::Result<PackageManifest, diag::Reported> parsed = parse_manifest(
        table, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
    CHECK(parsed.is_ok());
    if (!parsed.is_ok()) {
      continue;
    }
    const PackageManifest manifest = std::move(parsed).unwrap();
    CHECK(manifest.suite_only == nullptr);
    CHECK(manifest.suite_only_count == 0);
  }
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Manifest rejects bad [spec] tables") {
  const std::string head =
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\nlicense = \"\"\n";
  struct Case {
    std::string bytes;
    std::string_view message;
  };
  const Case cases[] = {
      {"spec = \"Index\"\n" + head,
       "Manifest 'alcy.toml': [spec] must be a table"},
      {head + "[spec]\nsuite-only = \"Index\"\n",
       "Manifest 'alcy.toml': [spec] suite-only must be a list of strings"},
      {head + "[spec]\nsuite-only = [\"Index\", 3]\n",
       "Manifest 'alcy.toml': [spec] suite-only must be a list of strings"},
      {head + "[spec]\nsuite-only = [\"Index\", \"\"]\n",
       "Manifest 'alcy.toml': [spec] suite-only must be a list of strings"},
      {head + "[spec]\nsuite-only = [\"Index\", \"Index\"]\n",
       "Manifest 'alcy.toml': duplicate [spec] suite-only entry 'Index'"},
      {head + "[spec]\nimplement = [\"Index\"]\n",
       "Manifest 'alcy.toml': [spec] implement is reserved and not accepted "
       "yet"},
      {head + "[spec]\nfrobnicate = []\n",
       "Manifest 'alcy.toml': unknown key in [spec]: 'frobnicate'"},
  };
  for (const Case& test_case : cases) {
    Fixture f;
    base::Result<PackageManifest, diag::Reported> result = parse_manifest(
        test_case.bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
    CHECK_MESSAGE(result.is_err(), test_case.bytes);
    CHECK(f.bag.has_errors());
    CHECK(f.bag.size() == 1);
    if (f.bag.size() != 1) {
      continue;
    }
    const diag::Diagnostic* const diag = f.bag.at(0);
    CHECK(diag != nullptr);
    if (diag == nullptr) {
      continue;
    }
    CHECK(diag->message == test_case.message);
  }
}

TEST_CASE("Suite manifest parses the std suite") {
  Fixture f;
  // The [suite] table of lib/std/alcy.toml, comments aside: the parser
  // must accept the suite the compiler embeds.
  constexpr std::string_view bytes =
      "[suite]\nname = \"std\"\nlicense = \"\"\nowner = \"alcy\"\n"
      "packages = [\"core\", \"fmt\", \"alloc\", \"collections\", \"atomic\", "
      "\"sync\", \"io\", \"network\", \"thread\", \"arch\", \"simd\", "
      "\"time\"]\n";
  base::Result<SuiteManifest, diag::Reported> result = parse_suite_manifest(
      bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const SuiteManifest manifest = std::move(result).unwrap();
  CHECK(!f.bag.has_errors());
  CHECK(manifest.owner == "alcy");
  CHECK(manifest.name == "std");
  CHECK(manifest.package_count == 12);
  if (manifest.package_count != 12) {
    return;
  }
  CHECK(manifest.packages[0] == "core");
  CHECK(manifest.packages[2] == "alloc");
  CHECK(manifest.packages[3] == "collections");
  CHECK(manifest.packages[11] == "time");
  CHECK(verify_suite_manifest(manifest).is_ok());

  // A fresh suite has no members and no owner yet (ADR-0057).
  constexpr std::string_view empty =
      "[suite]\nname = \"tools\"\nlicense = \"\"\npackages = []\n";
  base::Result<SuiteManifest, diag::Reported> empty_result =
      parse_suite_manifest(empty, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                           f.arena);
  CHECK(empty_result.is_ok());
  if (empty_result.is_ok()) {
    CHECK(verify_suite_manifest(std::move(empty_result).unwrap()).is_ok());
  }
}

TEST_CASE("Suite manifest rejects the wrong kind and bad shapes") {
  Fixture f;
  constexpr std::string_view package_manifest =
      "[package]\nname = \"hash\"\nversion = \"0.1.0\"\nlicense = \"\"\n";
  CHECK(parse_suite_manifest(package_manifest, "alcy.toml",
                             source::UNKNOWN_FILE, f.bag, f.arena)
            .is_err());
  constexpr std::string_view no_suite = "[other]\n";
  CHECK(parse_suite_manifest(no_suite, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                             f.arena)
            .is_err());
  constexpr std::string_view bad_owner =
      "[suite]\nowner = 1\nname = \"std\"\npackages = [\"core\"]\n";
  CHECK(parse_suite_manifest(bad_owner, "alcy.toml", source::UNKNOWN_FILE,
                             f.bag, f.arena)
            .is_err());
  constexpr std::string_view inherited_owner =
      "[suite]\nowner.suite = true\nname = \"std\"\n"
      "packages = [\"core\"]\n";
  CHECK(parse_suite_manifest(inherited_owner, "alcy.toml", source::UNKNOWN_FILE,
                             f.bag, f.arena)
            .is_err());
  constexpr std::string_view bad_version =
      "[suite]\nname = \"std\"\nlicense = \"\"\nversion = \"nope\"\n"
      "packages = [\"core\"]\n";
  CHECK(parse_suite_manifest(bad_version, "alcy.toml", source::UNKNOWN_FILE,
                             f.bag, f.arena)
            .is_err());
  constexpr std::string_view no_name =
      "[suite]\nowner = \"alcy\"\npackages = [\"core\"]\n";
  CHECK(parse_suite_manifest(no_name, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                             f.arena)
            .is_err());
  constexpr std::string_view no_packages =
      "[suite]\nowner = \"alcy\"\nname = \"std\"\nlicense = \"\"\n";
  CHECK(parse_suite_manifest(no_packages, "alcy.toml", source::UNKNOWN_FILE,
                             f.bag, f.arena)
            .is_err());
  constexpr std::string_view empty_entry =
      "[suite]\nowner = \"alcy\"\nname = \"std\"\nlicense = \"\"\n"
      "packages = [\"\"]\n";
  CHECK(parse_suite_manifest(empty_entry, "alcy.toml", source::UNKNOWN_FILE,
                             f.bag, f.arena)
            .is_err());
  constexpr std::string_view duplicate =
      "[suite]\nowner = \"alcy\"\nname = \"std\"\nlicense = \"\"\n"
      "packages = [\"core\", \"core\"]\n";
  CHECK(parse_suite_manifest(duplicate, "alcy.toml", source::UNKNOWN_FILE,
                             f.bag, f.arena)
            .is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Suite manifest verification rejects bad shapes") {
  CHECK(verify_suite_manifest(SuiteManifest{}).unwrap_err() ==
        SuiteError::EmptyName);
  // An owner-less, memberless suite is a valid starting point.
  SuiteManifest fresh;
  fresh.name = "tools";
  CHECK(verify_suite_manifest(fresh).is_ok());
  const std::string_view members[] = {"core", "core"};
  SuiteManifest duplicate;
  duplicate.name = "std";
  duplicate.packages = members;
  duplicate.package_count = 2;
  CHECK(verify_suite_manifest(duplicate).unwrap_err() ==
        SuiteError::DuplicatePackageEntry);
  const std::string_view same_name[] = {"a/core", "b/core"};
  SuiteManifest names;
  names.name = "std";
  names.packages = same_name;
  names.package_count = 2;
  CHECK(verify_suite_manifest(names).unwrap_err() ==
        SuiteError::DuplicatePackageName);
  const std::string_view trailing_slash[] = {"core/"};
  SuiteManifest bad_path;
  bad_path.name = "std";
  bad_path.packages = trailing_slash;
  bad_path.package_count = 1;
  CHECK(verify_suite_manifest(bad_path).unwrap_err() ==
        SuiteError::BadPackageEntry);
  Fixture f;
  report_suite_error(SuiteError::EmptyName, "alcy.toml", f.bag);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Package manifest names a suite manifest") {
  Fixture f;
  constexpr std::string_view bytes =
      "[suite]\nname = \"std\"\nlicense = \"\"\nowner = \"alcy\"\n"
      "packages = [\"core\"]\n";
  CHECK(parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena)
            .is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Manifest rejects binary targets without paths") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
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
  CHECK(diag->code.has_value());
  CHECK(diag->code.has_value());
  if (!diag->code.has_value()) {
    return;
  }
  CHECK(diag->code->stage == diag::Stage::Pkg);
  CHECK(diag->code->id == 1);
  CHECK(diag->has_primary_span);
  CHECK(diag->primary_span.file == 3);
}

// toml++ v3.4.0 called its key parser before checking the character that
// followed a table header's opening bracket; with `-fno-exceptions` that
// path asserts in debug and assumes the character is valid in release.
// The vendored fix rejects the byte first, so an unfinished header is a
// syntax error rather than a crash.
TEST_CASE("Manifest rejects an unfinished table header") {
  for (const std::string_view bytes : {"[\n", "[[\n", "[.\n", "[#\n"}) {
    Fixture f;
    base::Result<PackageManifest, diag::Reported> result =
        parse_manifest(bytes, "alcy.toml", 3, f.bag, f.arena);
    CHECK(result.is_err());
    CHECK(f.bag.has_errors());
    CHECK(f.bag.size() == 1);
    if (f.bag.size() != 1) {
      continue;
    }
    const diag::Diagnostic* const diag = f.bag.at(0);
    CHECK(diag != nullptr);
    if (diag == nullptr) {
      continue;
    }
    CHECK(diag->severity == diag::Severity::Error);
    CHECK(diag->code.has_value());
    if (!diag->code.has_value()) {
      continue;
    }
    CHECK(diag->code->stage == diag::Stage::Pkg);
    CHECK(diag->code->id == 1);
    CHECK(diag->has_primary_span);
    CHECK(diag->primary_span.file == 3);
  }
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
      "[package]\nname = \"x\"\nversion = \"nope\"\nlicense = \"\"\n";
  CHECK(parse_version("nope").is_err());
  CHECK(parse_manifest(bad_version, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                       f.arena)
            .is_err());
  constexpr std::string_view registry_dep =
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
      "[dependencies]\nfoo = \"1.0\"\n";
  CHECK(parse_manifest(registry_dep, "alcy.toml", source::UNKNOWN_FILE, f.bag,
                       f.arena)
            .is_err());
  constexpr std::string_view bad_req =
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
      "[dependencies]\nfoo = { version = \"1.2\" }\n";
  CHECK(
      parse_manifest(bad_req, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena)
          .is_err());
  CHECK(f.bag.error_count() == 5);
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

SuiteManifest parse_suite_ok(std::string_view bytes, Fixture& f) {
  base::Result<SuiteManifest, diag::Reported> result = parse_suite_manifest(
      bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  CHECK(!f.bag.has_errors());
  if (result.is_err()) {
    return SuiteManifest{};
  }
  return std::move(result).unwrap();
}

constexpr std::string_view DEPS_HEAD =
    "[package]\nname = \"x\"\nversion = \"0.1.0\"\nlicense = "
    "\"\"\n[dependencies]\n";

}  // namespace

TEST_CASE("Manifest parses suite and package specifiers") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\nlicense = "
      "\"\"\n[dependencies]\n"
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
      "[package]\nname = \"x\"\nversion = \"0.1.0\"\nlicense = "
      "\"\"\n[dependencies]\n"
      "\"acme/json\" = { version = \"1.x\" }\n"
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
  CHECK(manifest.dependencies[0].version_req.count == 1);
  CHECK(manifest.dependencies[0].version_req.bounds[0].op == VersionOp::Equal);
  CHECK(manifest.dependencies[0].version_req.bounds[0].version ==
        Version{2, 3, 4});
  CHECK(manifest.dependencies[1].version == "1.x");
  // A wildcard reaches the parser as the two bounds it stands for.
  CHECK(manifest.dependencies[1].version_req.count == 2);
  CHECK(manifest.dependencies[1].version_req.bounds[0].op ==
        VersionOp::GreaterEqual);
  CHECK(manifest.dependencies[1].version_req.bounds[0].version ==
        Version{1, 0, 0});
  CHECK(manifest.dependencies[1].version_req.bounds[1].op == VersionOp::Less);
  CHECK(manifest.dependencies[1].version_req.bounds[1].version ==
        Version{2, 0, 0});
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
  base::Result<Dependency, diag::Reported> valued = parse_dependency_flag(
      f.bag, f.arena, "acme/json = { version = \"1.x\" }");
  CHECK(valued.is_ok());
  if (valued.is_ok()) {
    const Dependency dep = std::move(valued).unwrap();
    CHECK(dep.owner == "acme");
    CHECK(dep.source == DependencySource::Registry);
    CHECK(dep.version == "1.x");
    CHECK(dep.version_req.count == 2);
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
      {"\"acme/x\" = { version = \"1.x\", git = \"https://e.com/r.git\" }\n"},
      {"\"acme/x\" = { version = \"1.x\", path = \"../x\" }\n"},
      {"\"acme/x\" = { version = \"^1.2.3\" }\n"},
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

// The `[modules]` table. Parsing it is this module's - deciding which
// files an entry selects is the pipeline's, and is tested there.
constexpr std::string_view MODULES_MANIFEST =
    "[package]\n"
    "name = \"demo\"\n"
    "version = \"0.1.0\"\n"
    "license = \"\"\n"
    "\n"
    "[[bin]]\n"
    "name = \"demo\"\n"
    "path = \"main.al\"\n";

TEST_CASE("Manifest parses explicit module sets") {
  Fixture f;
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(std::string(MODULES_MANIFEST) +
                         "\n[modules]\n"
                         "include = [\"main\", \"utils/io\"]\n"
                         "export = [\"api\"]\n",
                     "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (result.is_err()) {
    return;
  }
  const PackageManifest manifest = std::move(result).unwrap();
  CHECK(!manifest.modules.wildcard);
  CHECK(manifest.modules.include_count == 2);
  if (manifest.modules.include_count == 2) {
    CHECK(manifest.modules.include[0] == "main");
    CHECK(manifest.modules.include[1] == "utils/io");
  }
  CHECK(manifest.modules.export_count == 1);
  if (manifest.modules.export_count == 1) {
    CHECK(manifest.modules.exports[0] == "api");
  }
}

TEST_CASE("Manifest without modules selects wildcards") {
  Fixture f;
  base::Result<PackageManifest, diag::Reported> result = parse_manifest(
      MODULES_MANIFEST, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_ok());
  if (result.is_err()) {
    return;
  }
  const PackageManifest manifest = std::move(result).unwrap();
  CHECK(manifest.modules.wildcard);
  CHECK(manifest.modules.include_count == 0);
  CHECK(manifest.modules.export_count == 0);
}

TEST_CASE("Manifest rejects duplicate module entries") {
  Fixture f;
  base::Result<PackageManifest, diag::Reported> result =
      parse_manifest(std::string(MODULES_MANIFEST) +
                         "\n[modules]\n"
                         "include = [\"main\", \"main\"]\n",
                     "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena);
  CHECK(result.is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Package identity parses literals and suite markers") {
  Fixture f;
  constexpr std::string_view bytes =
      "[package]\nname = \"cli\"\nversion.suite = true\n"
      "owner.suite = true\nlicense = \"MIT\"\n";
  const PackageManifest manifest = parse_ok(bytes, f);
  CHECK(manifest.name == "cli");
  CHECK(!manifest.has_version);
  CHECK(manifest.version_from_suite);
  CHECK(manifest.owner_from_suite);
  CHECK(!manifest.license_from_suite);
  CHECK(manifest.license == "MIT");
  // Only resolution turns a marker into a value, so an unresolved
  // manifest does not cross the verifier.
  CHECK(verify_manifest(manifest).unwrap_err() ==
        ManifestError::UnresolvedInheritance);
}

TEST_CASE("A member takes the keys it spelled from its suite") {
  Fixture f;
  PackageManifest member = parse_ok(
      "[package]\nname = \"cli\"\nversion.suite = true\n"
      "owner.suite = true\nlicense.suite = true\n",
      f);
  const SuiteManifest suite = parse_suite_ok(
      "[suite]\nname = \"tools\"\nowner = \"acme\"\n"
      "version = \"1.2.3\"\nlicense = \"Apache-2.0\"\n"
      "packages = [\"cli\"]\n",
      f);
  CHECK(inherit_from_suite(member, suite).is_ok());
  CHECK(member.has_version);
  CHECK(member.version == Version{1, 2, 3});
  CHECK(member.owner == "acme");
  CHECK(member.license == "Apache-2.0");
  CHECK(verify_manifest(member).is_ok());
}

TEST_CASE("An inherited version needs a suite that declares one") {
  Fixture f;
  PackageManifest member = parse_ok(
      "[package]\nname = \"cli\"\nversion.suite = true\n"
      "license = \"\"\n",
      f);
  const SuiteManifest suite = parse_suite_ok(
      "[suite]\nname = \"tools\"\nlicense = \"\"\npackages = []\n", f);
  CHECK(inherit_from_suite(member, suite).unwrap_err() ==
        InheritError::SuiteVersionMissing);
}

TEST_CASE("Identity fields refuse shapes that are not a value or a marker") {
  for (const std::string_view bytes :
       {"[package]\nname = \"x\"\nversion.suite = false\n",
        "[package]\nname = \"x\"\nversion = \"0.1.0\"\nlicense = \"\"\n"
        "owner = { suite = true, extra = 1 }\n",
        "[package]\nname = \"x\"\nversion = \"0.1.0\"\nlicense = 1\n"}) {
    Fixture f;
    CHECK(
        parse_manifest(bytes, "alcy.toml", source::UNKNOWN_FILE, f.bag, f.arena)
            .is_err());
    CHECK(f.bag.has_errors());
  }
}

}  // namespace pkg
