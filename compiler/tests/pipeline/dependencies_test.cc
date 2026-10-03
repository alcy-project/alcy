// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/dependencies.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "path/path.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

namespace {

// Writes the package at `rel` with the manifest and files it
// names, so a case reads as the directory the compiler sees.
bool write_package(
    io::TempDir& dir,
    std::string_view rel,
    std::string_view manifest,
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        files) {
  if (!dir.write_file(std::string(rel) + "/alcy.toml", manifest)) {
    return false;
  }
  for (const auto& [name, bytes] : files) {
    if (!dir.write_file(std::string(rel) + "/" + std::string(name), bytes)) {
      return false;
    }
  }
  return true;
}

base::Result<CheckOutcome, diag::Reported> check_proj(PipelineContext& ctx,
                                                      const io::TempDir& dir) {
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(dir.join("proj"));
  if (root.is_err()) {
    return base::make_err(diag::Reported{});
  }
  const path::Path root_path = std::move(root).unwrap();
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(root_path.join("alcy.toml").as_view());
  if (manifest.is_err()) {
    return base::make_err(diag::Reported{});
  }
  return check_package(ctx, root_path, std::move(manifest).unwrap(),
                       "alcy.toml");
}

bool reports(const diag::DiagBag& bag, std::string_view message) {
  bool found = false;
  bag.for_each([&](const diag::Diagnostic& diagnostic) {
    found = found || diagnostic.message == message;
  });
  return found;
}

constexpr std::string_view HASH_MANIFEST =
    "[package]\nname = \"acme-hash\"\nversion = \"0.1.0\"\n\n"
    "[modules]\ninclude = [\"sha2\", \"detail\"]\n"
    "export = [\"sha2\"]\n\n"
    "[[bin]]\nname = \"acme-hash\"\npath = \"sha2.al\"\n";

}  // namespace

TEST_CASE("Dependencies spell an identity with underscores") {
  // The manifest name is what the package declares; the identity is
  // what a `use` spells, with `-` normalized to `_`.
  CHECK(package_identity("acme-hash") == "acme_hash");
  CHECK(package_identity("hash") == "hash");
}

TEST_CASE("Dependencies load a path dependency from source") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/hash\" = { path = \"vendor/hash\" }\n",
      {{"main.al",
        "use acme_hash::sha2::digest;\n\nfn main() -> i32 {\n  ret "
        "digest(1)\n}\n"}});
  const bool dep_setup = write_package(
      dir, "proj/vendor/hash", HASH_MANIFEST,
      {{"sha2.al", "pub fn digest(x: i32) -> i32 {\n  ret x + 1\n}\n"},
       {"detail.al", "pub fn helper() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (outcome.is_err()) {
    return;
  }
  // The closure checks as one world: the package's file and the
  // dependency's two.
  CHECK(std::move(outcome).unwrap().file_count == 3);
}

TEST_CASE("Dependencies resolve a dependency of a dependency") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/hash\" = { path = \"vendor/hash\" }\n",
      {{"main.al",
        "use acme_hash::sha2::digest;\n\nfn main() -> i32 {\n  ret "
        "digest(1)\n}\n"}});
  const bool hash_setup = write_package(
      dir, "proj/vendor/hash",
      "[package]\nname = \"acme-hash\"\nversion = \"0.1.0\"\n\n"
      "[modules]\ninclude = [\"sha2\"]\nexport = [\"sha2\"]\n\n"
      "[dependencies]\n\"acme/base\" = { path = \"../base\" }\n\n"
      "[[bin]]\nname = \"acme-hash\"\npath = \"sha2.al\"\n",
      {{"sha2.al",
        "use acme_base::util::twice;\n\npub fn digest(x: i32) -> i32 {\n  "
        "ret twice(x) + 1\n}\n"}});
  const bool base_setup = write_package(
      dir, "proj/vendor/base",
      "[package]\nname = \"acme-base\"\nversion = \"0.1.0\"\n\n"
      "[modules]\ninclude = [\"util\"]\nexport = [\"util\"]\n\n"
      "[[bin]]\nname = \"acme-base\"\npath = \"util.al\"\n",
      {{"util.al", "pub fn twice(x: i32) -> i32 {\n  ret x * 2\n}\n"}});
  CHECK(setup);
  CHECK(hash_setup);
  CHECK(base_setup);
  if (!setup || !hash_setup || !base_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (outcome.is_err()) {
    return;
  }
  CHECK(std::move(outcome).unwrap().file_count == 3);
}

TEST_CASE("Dependencies share a package two edges name") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/left\" = { path = \"vendor/left\" }\n"
      "\"acme/right\" = { path = \"vendor/right\" }\n",
      {{"main.al",
        "use acme_left::go::left;\nuse acme_right::go::right;\n\nfn "
        "main() -> i32 {\n  ret left() + right()\n}\n"}});
  const bool left_setup = write_package(
      dir, "proj/vendor/left",
      "[package]\nname = \"acme-left\"\nversion = \"0.1.0\"\n\n"
      "[modules]\ninclude = [\"go\"]\nexport = [\"go\"]\n\n"
      "[dependencies]\n\"acme/base\" = { path = \"../base\" }\n\n"
      "[[bin]]\nname = \"acme-left\"\npath = \"go.al\"\n",
      {{"go.al",
        "use acme_base::util::zero;\n\npub fn left() -> i32 {\n  ret "
        "zero()\n}\n"}});
  const bool right_setup = write_package(
      dir, "proj/vendor/right",
      "[package]\nname = \"acme-right\"\nversion = \"0.1.0\"\n\n"
      "[modules]\ninclude = [\"go\"]\nexport = [\"go\"]\n\n"
      "[dependencies]\n\"acme/base\" = { path = \"../base\" }\n\n"
      "[[bin]]\nname = \"acme-right\"\npath = \"go.al\"\n",
      {{"go.al",
        "use acme_base::util::zero;\n\npub fn right() -> i32 {\n  ret "
        "zero() + 1\n}\n"}});
  const bool base_setup =
      write_package(dir, "proj/vendor/base",
                    "[package]\nname = \"acme-base\"\nversion = \"0.1.0\"\n\n"
                    "[modules]\ninclude = [\"util\"]\nexport = [\"util\"]\n\n"
                    "[[bin]]\nname = \"acme-base\"\npath = \"util.al\"\n",
                    {{"util.al", "pub fn zero() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  CHECK(left_setup);
  CHECK(right_setup);
  CHECK(base_setup);
  if (!setup || !left_setup || !right_setup || !base_setup) {
    return;
  }

  // The shared package stages once, behind the one root both
  // edges reach: loading it twice would name two roots
  // 'acme_base', which the identity checks refuse.
  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (outcome.is_err()) {
    return;
  }
  CHECK(std::move(outcome).unwrap().file_count == 4);
}

TEST_CASE("Dependencies extend the selection through the closure") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/base\" = { path = \"vendor/base\" }\n",
      {{"main.al",
        "use acme_base::util::twice;\n\nfn main() -> i32 {\n  ret "
        "twice(1)\n}\n"}});
  const bool base_setup = write_package(
      dir, "proj/vendor/base",
      "[package]\nname = \"acme-base\"\nversion = \"0.1.0\"\n\n"
      "[modules]\ninclude = [\"util\"]\nexport = [\"util\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[[bin]]\nname = \"acme-base\"\npath = \"util.al\"\n",
      {{"util.al",
        "pub fn sz() -> u64 {\n  ret size_of::<i32>()\n}\n\npub fn "
        "twice(x: i32) -> i32 {\n  ret x * 2\n}\n"}});
  CHECK(setup);
  CHECK(base_setup);
  if (!setup || !base_setup) {
    return;
  }

  // The package never selects `alcy/std/core` itself; the
  // dependency's manifest does, and the closure's selection
  // stages it, so the dependency's `size_of` is in scope.
  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
}

TEST_CASE("Dependencies read the export list from the manifest") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/hash\" = { path = \"vendor/hash\" }\n",
      {{"main.al",
        "use acme_hash::detail::helper;\n\nfn main() -> i32 {\n  ret "
        "helper()\n}\n"}});
  const bool dep_setup = write_package(
      dir, "proj/vendor/hash", HASH_MANIFEST,
      {{"sha2.al", "pub fn digest(x: i32) -> i32 {\n  ret x + 1\n}\n"},
       {"detail.al", "pub fn helper() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(
      reports(ctx.bag, "Package 'acme_hash' does not export module 'detail'"));
}

TEST_CASE("Dependencies reject a cycle") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup =
      write_package(dir, "proj",
                    "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                    "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
                    "[dependencies]\n\"cy/a\" = { path = \"vendor/a\" }\n",
                    {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool a_setup =
      write_package(dir, "proj/vendor/a",
                    "[package]\nname = \"cy-a\"\nversion = \"0.1.0\"\n\n"
                    "[modules]\ninclude = [\"x\"]\n\n"
                    "[dependencies]\n\"cy/b\" = { path = \"../b\" }\n\n"
                    "[[bin]]\nname = \"cy-a\"\npath = \"x.al\"\n",
                    {{"x.al", "pub fn x() -> i32 {\n  ret 1\n}\n"}});
  const bool b_setup =
      write_package(dir, "proj/vendor/b",
                    "[package]\nname = \"cy-b\"\nversion = \"0.1.0\"\n\n"
                    "[modules]\ninclude = [\"y\"]\n\n"
                    "[dependencies]\n\"cy/a\" = { path = \"../a\" }\n\n"
                    "[[bin]]\nname = \"cy-b\"\npath = \"y.al\"\n",
                    {{"y.al", "pub fn y() -> i32 {\n  ret 2\n}\n"}});
  CHECK(setup);
  CHECK(a_setup);
  CHECK(b_setup);
  if (!setup || !a_setup || !b_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(
      ctx.bag, "Dependency cycle through '" + dir.join("proj/vendor/a") + "'"));
}

TEST_CASE("Dependencies reject two packages behind one identity") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup =
      write_package(dir, "proj",
                    "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                    "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
                    "[dependencies]\n\"acme/one\" = { path = \"vendor/one\" }\n"
                    "\"acme/two\" = { path = \"vendor/two\" }\n",
                    {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool one_setup =
      write_package(dir, "proj/vendor/one", HASH_MANIFEST,
                    {{"sha2.al", "pub fn one() -> i32 {\n  ret 1\n}\n"},
                     {"detail.al", "pub fn helper() -> i32 {\n  ret 0\n}\n"}});
  const bool two_setup =
      write_package(dir, "proj/vendor/two",
                    "[package]\nname = \"acme-hash\"\nversion = \"0.2.0\"\n\n"
                    "[modules]\ninclude = [\"sha2\"]\nexport = [\"sha2\"]\n\n"
                    "[[bin]]\nname = \"acme-hash\"\npath = \"sha2.al\"\n",
                    {{"sha2.al", "pub fn two() -> i32 {\n  ret 2\n}\n"}});
  CHECK(setup);
  CHECK(one_setup);
  CHECK(two_setup);
  if (!setup || !one_setup || !two_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag,
                "Two dependencies are named 'acme_hash'; rename "
                "one of them"));
}

TEST_CASE("Dependencies reject a package named like its dependent") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/app\" = { path = \"vendor/app\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup =
      write_package(dir, "proj/vendor/app",
                    "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                    "[modules]\ninclude = [\"x\"]\nexport = [\"x\"]\n\n"
                    "[[bin]]\nname = \"app\"\npath = \"x.al\"\n",
                    {{"x.al", "pub fn x() -> i32 {\n  ret 1\n}\n"}});
  CHECK(setup);
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag,
                "Dependency 'app' is named like the package that "
                "depends on it; rename one of them"));
}

TEST_CASE("Dependencies reject a package named like a staged member") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup =
      write_package(dir, "proj",
                    "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                    "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
                    "[dependencies]\n\"alcy/std/core\" = {}\n"
                    "\"acme/core\" = { path = \"vendor/core\" }\n",
                    {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup =
      write_package(dir, "proj/vendor/core",
                    "[package]\nname = \"core\"\nversion = \"0.1.0\"\n\n"
                    "[modules]\ninclude = [\"x\"]\nexport = [\"x\"]\n\n"
                    "[[bin]]\nname = \"core\"\npath = \"x.al\"\n",
                    {{"x.al", "pub fn x() -> i32 {\n  ret 1\n}\n"}});
  CHECK(setup);
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag,
                "Dependency 'core' is named like the standard "
                "library package alcy/std/core; rename one of "
                "them"));
}

TEST_CASE("Dependencies require a target of their own") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/none\" = { path = \"vendor/none\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup =
      write_package(dir, "proj/vendor/none",
                    "[package]\nname = \"no-target\"\nversion = \"0.1.0\"\n\n"
                    "[modules]\ninclude = [\"x\"]\n",
                    {{"x.al", "pub fn x() -> i32 {\n  ret 1\n}\n"}});
  CHECK(setup);
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag,
                "Manifest 'no-target' declares no [[bin]] or "
                "[lib] targets"));
}

TEST_CASE("Dependencies reject a name that is not one module path") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/bad\" = { path = \"vendor/bad\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup =
      write_package(dir, "proj/vendor/bad",
                    "[package]\nname = \"bad/name\"\nversion = \"0.1.0\"\n\n"
                    "[modules]\ninclude = [\"x\"]\n\n"
                    "[[bin]]\nname = \"bad\"\npath = \"x.al\"\n",
                    {{"x.al", "pub fn x() -> i32 {\n  ret 1\n}\n"}});
  CHECK(setup);
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag,
                "Dependency 'acme/bad' is named 'bad/name', which "
                "is not one module path"));
}

TEST_CASE("Dependencies report a directory without a manifest") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/gone\" = { path = \"vendor/nope\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  if (!setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag, "No manifest found at '" +
                             dir.join("proj/vendor/nope") +
                             "'; add alcy.toml"));
}

}  // namespace pipeline
