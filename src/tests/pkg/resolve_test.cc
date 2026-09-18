// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "pkg/resolve.h"

#include <ostream>  // IWYU pragma: keep (required for doctest's CHECK macro on windows)
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/mem/arena.h"
#include "pkg/manifest.h"
#include "source/source.h"
#include "tests/util/test_fs.h"

namespace pkg {

namespace {

constexpr std::string_view kRootManifest =
    "[package]\n"
    "name = \"root\"\n"
    "version = \"0.1.0\"\n"
    "\n"
    "[dependencies]\n"
    "leaf = { path = \"libs/leaf\" }\n"
    "mid = { path = \"mid\" }\n";

constexpr std::string_view kMidManifest =
    "[package]\n"
    "name = \"mid\"\n"
    "version = \"0.2.0\"\n"
    "\n"
    "[dependencies]\n"
    "leaf = { path = \"../libs/leaf\" }\n";

constexpr std::string_view kLeafManifest =
    "[package]\n"
    "name = \"leaf\"\n"
    "version = \"0.3.0\"\n";

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena};
  source::SourceManager sources;

  Fixture() { arena.reserve(1u << 20); }
};

bool write_package(test_fs::TempDir& dir,
                   std::string_view rel,
                   std::string_view manifest,
                   std::string& out_root) {
  const std::string manifest_path =
      std::string(rel) + "/" + std::string(kManifestFileName);
  if (!dir.write_file(manifest_path, manifest)) {
    return false;
  }
  out_root = dir.join(rel);
  return true;
}

}  // namespace

TEST_CASE("Resolve collects transitive path dependencies") {
  test_fs::TempDir dir("alcy_resolve_test");
  std::string root;
  bool setup = write_package(dir, "root", kRootManifest, root);
  setup = setup && dir.write_file("root/libs/leaf/alcy.toml", kLeafManifest);
  setup = setup && dir.write_file("root/mid/alcy.toml", kMidManifest);
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  diag::Fallible<std::vector<ResolvedPackage>> result =
      resolve_package(root, f.sources, f.arena, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const std::vector<ResolvedPackage> resolved = std::move(result).unwrap();
  CHECK(!f.bag.has_errors());
  // Sorted depth-first: root, leaf (root's dep), mid, leaf (mid's dep again;
  // no dedup in MVP).
  CHECK(resolved.size() == 4);
  if (resolved.size() != 4) {
    return;
  }
  CHECK(resolved[0].manifest.name == "root");
  CHECK(resolved[1].manifest.name == "leaf");
  CHECK(resolved[2].manifest.name == "mid");
  CHECK(resolved[3].manifest.name == "leaf");
  CHECK(resolved[0].manifest_file != source::kUnknownFile);
}

TEST_CASE("Resolve detects dependency cycles across spellings") {
  test_fs::TempDir dir("alcy_resolve_cycle_test");
  constexpr std::string_view a_manifest =
      "[package]\nname = \"a\"\nversion = \"0.1.0\"\n"
      "[dependencies]\nb = { path = \"../b\" }\n";
  constexpr std::string_view b_manifest =
      "[package]\nname = \"b\"\nversion = \"0.1.0\"\n"
      "[dependencies]\na = { path = \"./../a/./\" }\n";
  std::string root;
  bool setup = write_package(dir, "a", a_manifest, root);
  setup = setup && dir.write_file("b/alcy.toml", b_manifest);
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(resolve_package(root, f.sources, f.arena, f.bag).is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Resolve visits diamonds twice without cycle errors") {
  test_fs::TempDir dir("alcy_resolve_diamond_test");
  constexpr std::string_view top_manifest =
      "[package]\nname = \"top\"\nversion = \"0.1.0\"\n"
      "[dependencies]\nb = { path = \"b\" }\nc = { path = \"c\" }\n";
  constexpr std::string_view mid_manifest =
      "[package]\nname = \"m\"\nversion = \"0.1.0\"\n"
      "[dependencies]\nd = { path = \"../d\" }\n";
  constexpr std::string_view d_manifest =
      "[package]\nname = \"d\"\nversion = \"0.1.0\"\n";
  std::string root;
  bool setup = write_package(dir, "top", top_manifest, root);
  setup = setup && dir.write_file("top/b/alcy.toml", mid_manifest);
  setup = setup && dir.write_file("top/c/alcy.toml", mid_manifest);
  setup = setup && dir.write_file("top/d/alcy.toml", d_manifest);
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  diag::Fallible<std::vector<ResolvedPackage>> result =
      resolve_package(root, f.sources, f.arena, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  CHECK(!f.bag.has_errors());
  // Shared dependencies resolve once per incoming edge (no dedup in MVP).
  CHECK(std::move(result).unwrap().size() == 5);
}

TEST_CASE("Resolve reports missing manifests") {
  test_fs::TempDir dir("alcy_resolve_missing_test");
  const bool setup = dir.make_dir("empty");
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  CHECK(resolve_package(dir.join("empty"), f.sources, f.arena, f.bag).is_err());
  CHECK(f.bag.has_errors());
}

}  // namespace pkg
