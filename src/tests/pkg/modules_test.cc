// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "pkg/modules.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "fpag/mem/arena.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena};
  source::SourceManager sources;

  Fixture() { arena.reserve(1u << 20); }
};

constexpr std::string_view kBaseManifest =
    "[package]\n"
    "name = \"demo\"\n"
    "version = \"0.1.0\"\n"
    "\n"
    "[[bin]]\n"
    "name = \"demo\"\n"
    "path = \"main.al\"\n";

PackageManifest parse_ok(std::string_view text, Fixture& f) {
  diag::Fallible<PackageManifest> result =
      parse_manifest(text, "alcy.toml", source::kUnknownFile, f.bag, f.arena);
  CHECK(result.is_ok());
  if (result.is_err()) {
    return PackageManifest{};
  }
  CHECK(!f.bag.has_errors());
  return std::move(result).unwrap();
}

bool write_all(io::TempDir& dir, std::string_view rel, std::string_view text) {
  return dir.write_file(rel, text);
}

}  // namespace

TEST_CASE("Manifest parses explicit module sets") {
  Fixture f;
  const PackageManifest manifest =
      parse_ok(std::string(kBaseManifest) +
                   "\n[modules]\n"
                   "include = [\"main\", \"utils/io\"]\n"
                   "export = [\"api\"]\n",
               f);
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
  const PackageManifest manifest = parse_ok(std::string(kBaseManifest), f);
  CHECK(manifest.modules.wildcard);
  CHECK(manifest.modules.include_count == 0);
  CHECK(manifest.modules.export_count == 0);
}

TEST_CASE("Manifest rejects duplicate module entries") {
  Fixture f;
  diag::Fallible<PackageManifest> result =
      parse_manifest(std::string(kBaseManifest) +
                         "\n[modules]\n"
                         "include = [\"main\", \"main\"]\n",
                     "alcy.toml", source::kUnknownFile, f.bag, f.arena);
  CHECK(result.is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Modules resolve explicit entries to files") {
  io::TempDir dir("alcy_modules_explicit_test");
  const bool setup =
      write_all(dir, "main.al", "fn main() {}\n") &&
      write_all(dir, "util.al", "fn double(x: i32) -> i32 {\n  ret x\n}\n") &&
      write_all(dir, "extra.al", "fn unused() {}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const PackageManifest manifest =
      parse_ok(std::string(kBaseManifest) +
                   "\n[modules]\n"
                   "include = [\"main\", \"util\"]\n",
               f);
  std::vector<source::FileId> files;
  for (std::string_view rel : {"main.al", "util.al", "extra.al"}) {
    base::Result<source::FileId, source::SourceError> loaded =
        f.sources.load(dir.join(rel));
    CHECK(loaded.is_ok());
    if (loaded.is_err()) {
      return;
    }
    files.push_back(std::move(loaded).unwrap());
  }
  diag::Fallible<std::vector<ModuleFile>> resolved = resolve_module_files(
      manifest, dir.path(), files, f.sources, f.bag, f.arena);
  CHECK(resolved.is_ok());
  if (resolved.is_err()) {
    return;
  }
  const std::vector<ModuleFile> selected = std::move(resolved).unwrap();
  CHECK(selected.size() == 2);
  if (selected.size() != 2) {
    return;
  }
  CHECK(selected[0].name == "main");
  CHECK(selected[1].name == "util");
  CHECK(!f.bag.has_errors());
  // extra.al is discovered but unselected.
  CHECK(f.bag.warning_count() > 0);
}

TEST_CASE("Modules resolve wildcards by relative path") {
  io::TempDir dir("alcy_modules_wildcard_test");
  const bool setup = write_all(dir, "main.al", "fn main() {}\n") &&
                     dir.make_dir("io") &&
                     write_all(dir, "io/util.al", "fn helper() {}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const PackageManifest manifest = parse_ok(std::string(kBaseManifest), f);
  CHECK(manifest.modules.wildcard);
  std::vector<source::FileId> files;
  for (std::string_view rel : {"main.al", "io/util.al"}) {
    base::Result<source::FileId, source::SourceError> loaded =
        f.sources.load(dir.join(rel));
    CHECK(loaded.is_ok());
    if (loaded.is_err()) {
      return;
    }
    files.push_back(std::move(loaded).unwrap());
  }
  // The nested file needs its directory to exist first.
  diag::Fallible<std::vector<ModuleFile>> resolved = resolve_module_files(
      manifest, dir.path(), files, f.sources, f.bag, f.arena);
  CHECK(resolved.is_ok());
  if (resolved.is_err()) {
    return;
  }
  const std::vector<ModuleFile> selected = std::move(resolved).unwrap();
  CHECK(selected.size() == 2);
  if (selected.size() != 2) {
    return;
  }
  CHECK(selected[0].name == "main");
  CHECK(selected[1].name == "io/util");
}

TEST_CASE("Modules reject missing include files") {
  io::TempDir dir("alcy_modules_missing_test");
  const bool setup = write_all(dir, "main.al", "fn main() {}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  Fixture f;
  const PackageManifest manifest =
      parse_ok(std::string(kBaseManifest) +
                   "\n[modules]\n"
                   "include = [\"main\", \"ghost\"]\n",
               f);
  std::vector<source::FileId> files;
  base::Result<source::FileId, source::SourceError> loaded =
      f.sources.load(dir.join("main.al"));
  CHECK(loaded.is_ok());
  if (loaded.is_err()) {
    return;
  }
  files.push_back(std::move(loaded).unwrap());
  diag::Fallible<std::vector<ModuleFile>> resolved = resolve_module_files(
      manifest, dir.path(), files, f.sources, f.bag, f.arena);
  CHECK(resolved.is_err());
  CHECK(f.bag.has_errors());
}

}  // namespace pkg
