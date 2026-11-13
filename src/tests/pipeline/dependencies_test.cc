// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/dependencies.h"

#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen/backend.h"
#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "ir/block.h"
#include "ir/common.h"
#include "ir/deserialize.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/type.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/build_plan.h"
#include "pipeline/check.h"
#include "pipeline/emit_mode.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/spawn.h"
#include "pkg/manifest.h"
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
    "[package]\nname = \"acme-hash\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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

TEST_CASE("Dependencies stage once per run") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/hash\" = { path = \"vendor/hash\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup = write_package(
      dir, "proj/vendor/hash", HASH_MANIFEST,
      {{"sha2.al", "pub fn digest(x: i32) -> i32 {\n  ret x\n}\n"},
       {"detail.al", "pub fn helper() -> i32 {\n  ret 0\n}\n"}});
  CHECK(setup);
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(dir.join("proj"));
  CHECK(root.is_ok());
  if (root.is_err()) {
    return;
  }
  const path::Path root_path = std::move(root).unwrap();
  base::Result<source::FileId, source::SourceError> manifest_file =
      ctx.sources.load(root_path.join("alcy.toml").as_view());
  CHECK(manifest_file.is_ok());
  if (manifest_file.is_err()) {
    return;
  }
  const source::FileId file = std::move(manifest_file).unwrap();
  base::Result<pkg::PackageManifest, diag::Reported> parsed =
      pkg::parse_manifest(ctx.sources.bytes(file).value_or(std::string_view{}),
                          "alcy.toml", file, ctx.bag, ctx.arena);
  CHECK(parsed.is_ok());
  if (parsed.is_err()) {
    return;
  }
  const pkg::PackageManifest manifest = std::move(parsed).unwrap();

  base::Result<DependencyClosure, diag::Reported> first =
      resolve_dependencies(ctx, root_path, manifest);
  CHECK(first.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (first.is_err()) {
    return;
  }
  const DependencyClosure once = std::move(first).unwrap();
  CHECK(once.size() == 1);
  CHECK(once[0].manifest.name == "acme-hash");

  // A second resolution takes the entry the run's table already holds,
  // which is what lets a suite's members share their dependencies.
  base::Result<DependencyClosure, diag::Reported> again =
      resolve_dependencies(ctx, root_path, manifest);
  CHECK(again.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (again.is_err()) {
    return;
  }
  const DependencyClosure twice = std::move(again).unwrap();
  CHECK(twice.size() == 1);
  CHECK(&once[0] == &twice[0]);
}

TEST_CASE("Dependencies resolve a dependency of a dependency") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/hash\" = { path = \"vendor/hash\" }\n",
      {{"main.al",
        "use acme_hash::sha2::digest;\n\nfn main() -> i32 {\n  ret "
        "digest(1)\n}\n"}});
  const bool hash_setup = write_package(
      dir, "proj/vendor/hash",
      "[package]\nname = \"acme-hash\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"sha2\"]\nexport = [\"sha2\"]\n\n"
      "[dependencies]\n\"acme/base\" = { path = \"../base\" }\n\n"
      "[[bin]]\nname = \"acme-hash\"\npath = \"sha2.al\"\n",
      {{"sha2.al",
        "use acme_base::util::twice;\n\npub fn digest(x: i32) -> i32 {\n  "
        "ret twice(x) + 1\n}\n"}});
  const bool base_setup = write_package(
      dir, "proj/vendor/base",
      "[package]\nname = \"acme-base\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/left\" = { path = \"vendor/left\" }\n"
      "\"acme/right\" = { path = \"vendor/right\" }\n",
      {{"main.al",
        "use acme_left::go::left;\nuse acme_right::go::right;\n\nfn "
        "main() -> i32 {\n  ret left() + right()\n}\n"}});
  const bool left_setup = write_package(
      dir, "proj/vendor/left",
      "[package]\nname = \"acme-left\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"go\"]\nexport = [\"go\"]\n\n"
      "[dependencies]\n\"acme/base\" = { path = \"../base\" }\n\n"
      "[[bin]]\nname = \"acme-left\"\npath = \"go.al\"\n",
      {{"go.al",
        "use acme_base::util::zero;\n\npub fn left() -> i32 {\n  ret "
        "zero()\n}\n"}});
  const bool right_setup = write_package(
      dir, "proj/vendor/right",
      "[package]\nname = \"acme-right\"\nversion = \"0.1.0\"\nlicense = "
      "\"\"\n\n"
      "[modules]\ninclude = [\"go\"]\nexport = [\"go\"]\n\n"
      "[dependencies]\n\"acme/base\" = { path = \"../base\" }\n\n"
      "[[bin]]\nname = \"acme-right\"\npath = \"go.al\"\n",
      {{"go.al",
        "use acme_base::util::zero;\n\npub fn right() -> i32 {\n  ret "
        "zero() + 1\n}\n"}});
  const bool base_setup = write_package(
      dir, "proj/vendor/base",
      "[package]\nname = \"acme-base\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/base\" = { path = \"vendor/base\" }\n",
      {{"main.al",
        "use acme_base::util::twice;\n\nfn main() -> i32 {\n  ret "
        "twice(1)\n}\n"}});
  const bool base_setup = write_package(
      dir, "proj/vendor/base",
      "[package]\nname = \"acme-base\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"util\"]\nexport = [\"util\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[[bin]]\nname = \"acme-base\"\npath = \"util.al\"\n",
      {{"util.al",
        "pub fn sz() -> usize {\n  ret size_of::<i32>()\n}\n\npub fn "
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"cy/a\" = { path = \"vendor/a\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool a_setup = write_package(
      dir, "proj/vendor/a",
      "[package]\nname = \"cy-a\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"x\"]\n\n"
      "[dependencies]\n\"cy/b\" = { path = \"../b\" }\n\n"
      "[[bin]]\nname = \"cy-a\"\npath = \"x.al\"\n",
      {{"x.al", "pub fn x() -> i32 {\n  ret 1\n}\n"}});
  const bool b_setup = write_package(
      dir, "proj/vendor/b",
      "[package]\nname = \"cy-b\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/one\" = { path = \"vendor/one\" }\n"
      "\"acme/two\" = { path = \"vendor/two\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool one_setup =
      write_package(dir, "proj/vendor/one", HASH_MANIFEST,
                    {{"sha2.al", "pub fn one() -> i32 {\n  ret 1\n}\n"},
                     {"detail.al", "pub fn helper() -> i32 {\n  ret 0\n}\n"}});
  const bool two_setup = write_package(
      dir, "proj/vendor/two",
      "[package]\nname = \"acme-hash\"\nversion = \"0.2.0\"\nlicense = \"\"\n\n"
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/app\" = { path = \"vendor/app\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup = write_package(
      dir, "proj/vendor/app",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "\"acme/core\" = { path = \"vendor/core\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup = write_package(
      dir, "proj/vendor/core",
      "[package]\nname = \"core\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/none\" = { path = \"vendor/none\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup = write_package(
      dir, "proj/vendor/none",
      "[package]\nname = \"no-target\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/bad\" = { path = \"vendor/bad\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup = write_package(
      dir, "proj/vendor/bad",
      "[package]\nname = \"bad/name\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
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

constexpr std::string_view TOOLS_SUITE_MANIFEST =
    "[suite]\nowner = \"acme\"\nname = \"tools\"\nlicense = \"\"\n"
    "packages = [\"cli\", \"fmt\"]\n";

constexpr std::string_view CLI_MANIFEST =
    "[package]\nname = \"cli\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[modules]\ninclude = [\"run\"]\nexport = [\"run\"]\n\n"
    "[[bin]]\nname = \"cli\"\npath = \"run.al\"\n";

constexpr std::string_view FMT_MANIFEST =
    "[package]\nname = \"fmt\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[modules]\ninclude = [\"show\"]\nexport = [\"show\"]\n\n"
    "[[bin]]\nname = \"fmt\"\npath = \"show.al\"\n";

// Writes the two-member suite every suite case below resolves
// through: the `cli` and `fmt` packages under vendor/tools.
bool write_tools_suite(io::TempDir& dir) {
  return write_package(dir, "proj/vendor/tools", TOOLS_SUITE_MANIFEST, {}) &&
         write_package(dir, "proj/vendor/tools/cli", CLI_MANIFEST,
                       {{"run.al", "pub fn go() -> i32 {\n  ret 20\n}\n"}}) &&
         write_package(
             dir, "proj/vendor/tools/fmt", FMT_MANIFEST,
             {{"show.al", "pub fn shout(x: i32) -> i32 {\n  ret x * 2\n}\n"}});
}

TEST_CASE("Suites load every member a glob selects") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/*\" = { path = \"vendor/tools\" }\n",
      {{"main.al",
        "use cli::run::go;\nuse fmt::show::shout;\n\nfn main() "
        "-> i32 {\n  ret go() + shout(1)\n}\n"}});
  const bool suite_setup = write_tools_suite(dir);
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (outcome.is_err()) {
    return;
  }
  // The package's file and one file per member.
  CHECK(std::move(outcome).unwrap().file_count == 3);
}

TEST_CASE("Suites load one member a specifier names") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/cli\" = { path = \"vendor/tools\" }\n",
      {{"main.al",
        "use cli::run::go;\n\nfn main() -> i32 {\n  ret go()\n}\n"}});
  const bool suite_setup = write_tools_suite(dir);
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (outcome.is_err()) {
    return;
  }
  // Only the named member loads: `fmt` stays out of the closure.
  CHECK(std::move(outcome).unwrap().file_count == 2);
}

TEST_CASE("Suites share members two specifiers name") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/*\" = { path = \"vendor/tools\" }\n"
      "\"acme/tools/cli\" = { path = \"vendor/tools\" }\n",
      {{"main.al",
        "use cli::run::go;\nuse fmt::show::shout;\n\nfn main() "
        "-> i32 {\n  ret go() + shout(1)\n}\n"}});
  const bool suite_setup = write_tools_suite(dir);
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  // The suite reads twice, but each member stages once: naming
  // `cli` beside the glob is not two packages behind one
  // identity.
  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (outcome.is_err()) {
    return;
  }
  CHECK(std::move(outcome).unwrap().file_count == 3);
}

TEST_CASE("Suites reject a member the suite does not list") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/nope\" = { path = \"vendor/tools\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool suite_setup = write_tools_suite(dir);
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag,
                "Dependency 'acme/tools/nope' is not a member of "
                "suite 'acme/tools'"));
}

TEST_CASE("Suites reject a specifier the manifest does not name") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"other/stuff/cli\" = { path = \"vendor/tools\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool suite_setup = write_tools_suite(dir);
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag,
                "Dependency 'other/stuff/cli' names suite "
                "'other/stuff', but '" +
                    dir.join("proj/vendor/tools") +
                    "' holds suite 'acme/tools'"));
}

TEST_CASE("A suite member delivered by path takes its inherited keys") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_marker_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/cli\" = { path = \"vendor/tools\" }\n",
      {{"main.al",
        "use cli::run::go;\n\nfn main() -> i32 {\n  ret go()\n}\n"}});
  const bool suite_setup =
      write_package(dir, "proj/vendor/tools",
                    "[suite]\nowner = \"acme\"\nname = \"tools\"\n"
                    "version = \"1.2.3\"\nlicense = \"MIT\"\n"
                    "packages = [\"cli\"]\n",
                    {}) &&
      write_package(dir, "proj/vendor/tools/cli",
                    "[package]\nname = \"cli\"\nversion.suite = true\n"
                    "license.suite = true\n\n"
                    "[modules]\ninclude = [\"run\"]\nexport = [\"run\"]\n\n"
                    "[[bin]]\nname = \"cli\"\npath = \"run.al\"\n",
                    {{"run.al", "pub fn go() -> i32 {\n  ret 20\n}\n"}});
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  // The verifier sees the member only after its suite resolved the
  // markers it spelled.
  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
}

TEST_CASE("A suite member is addressed by the last path segment") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/cli\" = { path = \"vendor/tools\" }\n",
      {{"main.al",
        "use cli::run::go;\n\nfn main() -> i32 {\n  ret go()\n}\n"}});
  const bool suite_setup =
      write_package(dir, "proj/vendor/tools",
                    "[suite]\nowner = \"acme\"\nname = \"tools\"\n"
                    "license = \"\"\npackages = [\"first-party/cli\"]\n",
                    {}) &&
      write_package(dir, "proj/vendor/tools/first-party/cli", CLI_MANIFEST,
                    {{"run.al", "pub fn go() -> i32 {\n  ret 20\n}\n"}});
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  // The address names the package; the suite keeps the path.
  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_proj(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
}

TEST_CASE("A member's manifest must carry the name it is listed by") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/cli\" = { path = \"vendor/tools\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool suite_setup =
      write_package(dir, "proj/vendor/tools",
                    "[suite]\nowner = \"acme\"\nname = \"tools\"\n"
                    "license = \"\"\npackages = [\"first-party/cli\"]\n",
                    {}) &&
      write_package(dir, "proj/vendor/tools/first-party/cli",
                    "[package]\nname = \"other\"\nversion = \"0.1.0\"\n"
                    "license = \"\"\n\n"
                    "[modules]\ninclude = [\"run\"]\nexport = [\"run\"]\n\n"
                    "[[bin]]\nname = \"other\"\npath = \"run.al\"\n",
                    {{"run.al", "pub fn go() -> i32 {\n  ret 20\n}\n"}});
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag,
                "Dependency 'acme/tools/cli' declares package 'other', "
                "but its suite lists it as 'cli'"));
}

TEST_CASE("Suites read a package manifest as the wrong kind") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/cli\" = { path = \"vendor/solo\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool dep_setup = write_package(
      dir, "proj/vendor/solo",
      "[package]\nname = \"solo\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"x\"]\nexport = [\"x\"]\n\n"
      "[[bin]]\nname = \"solo\"\npath = \"x.al\"\n",
      {{"x.al", "pub fn x() -> i32 {\n  ret 1\n}\n"}});
  CHECK(setup);
  CHECK(dep_setup);
  if (!setup || !dep_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag, "Manifest '" + dir.join("proj/vendor/solo/alcy.toml") +
                             "': is a package manifest, not a suite "
                             "manifest"));
}

TEST_CASE("Suites read a suite manifest as the wrong kind") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools\" = { path = \"vendor/tools\" }\n",
      {{"main.al", "fn main() -> i32 {\n  ret 0\n}\n"}});
  const bool suite_setup = write_tools_suite(dir);
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  // A two-segment specifier names a package, so a suite
  // directory is the wrong kind rather than a member lookup.
  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag, "Manifest '" +
                             dir.join("proj/vendor/tools/alcy.toml") +
                             "': is a suite manifest, not a package "
                             "manifest"));
}

TEST_CASE("Suites resolve a member's own dependencies") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/cli\" = { path = \"vendor/tools\" }\n",
      {{"main.al",
        "use cli::run::go;\n\nfn main() -> i32 {\n  ret go()\n}\n"}});
  const bool suite_setup =
      write_package(dir, "proj/vendor/tools", TOOLS_SUITE_MANIFEST, {}) &&
      write_package(
          dir, "proj/vendor/tools/cli",
          "[package]\nname = \"cli\"\nversion = \"0.1.0\"\nlicense = "
          "\"\"\n\n"
          "[modules]\ninclude = [\"run\"]\nexport = [\"run\"]\n\n"
          "[dependencies]\n\"acme/solo\" = { path = \"../../solo\" }\n\n"
          "[[bin]]\nname = \"cli\"\npath = \"run.al\"\n",
          {{"run.al",
            "use solo::x::x;\n\npub fn go() -> i32 {\n  ret x() + 1\n}\n"}}) &&
      write_package(
          dir, "proj/vendor/tools/fmt", FMT_MANIFEST,
          {{"show.al", "pub fn shout(x: i32) -> i32 {\n  ret x * 2\n}\n"}});
  const bool solo_setup = write_package(
      dir, "proj/vendor/solo",
      "[package]\nname = \"solo\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"x\"]\nexport = [\"x\"]\n\n"
      "[[bin]]\nname = \"solo\"\npath = \"x.al\"\n",
      {{"x.al", "pub fn x() -> i32 {\n  ret 1\n}\n"}});
  CHECK(setup);
  CHECK(suite_setup);
  CHECK(solo_setup);
  if (!setup || !suite_setup || !solo_setup) {
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

TEST_CASE("Suites reject a member resolving back into its suite") {
  io::TempDir dir = io::TempDir::create_unique("alcy_dep_test_");
  const bool setup = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n\n"
      "[dependencies]\n\"acme/tools/*\" = { path = \"vendor/tools\" }\n",
      {{"main.al",
        "use cli::run::go;\n\nfn main() -> i32 {\n  ret go()\n}\n"}});
  const bool suite_setup =
      write_package(dir, "proj/vendor/tools", TOOLS_SUITE_MANIFEST, {}) &&
      write_package(dir, "proj/vendor/tools/cli",
                    "[package]\nname = \"cli\"\nversion = "
                    "\"0.1.0\"\nlicense = \"\"\n\n"
                    "[modules]\ninclude = [\"run\"]\nexport = [\"run\"]\n\n"
                    "[dependencies]\n\"acme/tools/*\" = { path = \"../\" }\n\n"
                    "[[bin]]\nname = \"cli\"\npath = \"run.al\"\n",
                    {{"run.al", "pub fn go() -> i32 {\n  ret 1\n}\n"}}) &&
      write_package(
          dir, "proj/vendor/tools/fmt", FMT_MANIFEST,
          {{"show.al", "pub fn shout(x: i32) -> i32 {\n  ret x * 2\n}\n"}});
  CHECK(setup);
  CHECK(suite_setup);
  if (!setup || !suite_setup) {
    return;
  }

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_proj(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  CHECK(reports(ctx.bag, "Dependency cycle through '" +
                             dir.join("proj/vendor/tools") + "'"));
}

TEST_CASE("A generic item a summary cannot spell is named for what it is") {
  // A summary carries no generic item's body yet, so a dependent that
  // names one is told that the item is generic rather than that it
  // does not exist.
  io::TempDir dir = io::TempDir::create_unique("alcy_summary_generic_test_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al",
        "pub struct Holder<T> {\n  pub value: T,\n}\n\n"
        "pub fn make(x: i32) -> i32 {\n  ret x\n}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join("dep/out/dep.summary")));
  }
  // The library's sources are gone, so the summary answers for it.
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  CHECK(dir.write_file(
      "proj/alcy.toml",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\nname = \"app\"\npath = \"main.al\"\n"));
  CHECK(dir.write_file("proj/main.al",
                       "use dep::Holder\n\nfn main() -> i32 {\n  ret 0\n}\n"));
  {
    PipelineContext ctx{i18n::Language::EnUs};
    const base::Result<CheckOutcome, diag::Reported> checked =
        check_proj(ctx, dir);
    CHECK(checked.is_err());
    CHECK(reports(ctx.bag,
                  "Item 'Holder' is generic; a summary does not carry a "
                  "generic item's body yet"));
  }
}

TEST_CASE("A summary carries a generic body the dependent instantiates") {
  // The summary names a generic item's definition-site body, and a
  // dependent's call clones it beside its own code (ADR-0079).
  io::TempDir dir =
      io::TempDir::create_unique("alcy_summary_generic_body_test_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al",
        "pub fn identity<T>(x: T) -> T {\n  ret x\n}\n\n"
        "pub fn double(x: i32) -> i32 {\n  ret x + x\n}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  // Build the library once, so the summary is written.
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join("dep/out/dep.summary")));
  }
#if !BUILD_FLAG(IS_OS_ASMJS)
  // The object a dependent links comes from the same build.
  {
    PipelineContext ctx{i18n::Language::EnUs};
    ctx.backend = codegen::Backend::Llvm;
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::Object);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join("dep/out/dep.o")));
  }
#endif  // !BUILD_FLAG(IS_OS_ASMJS)
  // The library's sources are gone, so the summary answers for it.
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  const bool app_written = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\npath = \"main.al\"\n",
      {{"main.al",
        "use dep::identity\n\nfn main() -> i32 {\n"
        "  ret identity(21i32) - 21\n}\n"}});
  CHECK(app_written);
  if (!app_written) {
    return;
  }
  // The dependent's build merges the carried body and the
  // specialization clones it; an ir build reaches both.
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("proj"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
  }
#if !BUILD_FLAG(IS_OS_ASMJS)
  // The linked program runs the carried body's clone.
  {
    PipelineContext ctx{i18n::Language::EnUs};
    ctx.backend = codegen::Backend::Llvm;
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("proj"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::Executable);
    CHECK(built.is_ok());
    CHECK(!ctx.bag.has_errors());
    const std::string exe =
        dir.join("proj/out/app" + std::string(exe_suffix()));
    CHECK(io::is_file(exe));
    base::Result<i32, SpawnError> ran = run_command({exe});
    CHECK(ran.is_ok());
    if (ran.is_ok()) {
      CHECK(std::move(ran).unwrap() == 0);
    }
  }
#endif  // !BUILD_FLAG(IS_OS_ASMJS)
}

TEST_CASE("A summary's generic declaration and impl cross whole") {
  // A generic struct's declaration, a generic impl's methods, and a
  // free generic that names them all cross in one summary, and a
  // dependent instantiates each beside its own code (ADR-0079).
  io::TempDir dir = io::TempDir::create_unique("alcy_summary_generic_full_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al",
        "pub struct Box<T> {\n  pub value: T,\n}\n\n"
        "impl<T> Box<T> {\n"
        "  pub fn wrap(value: T) -> Box<T> {\n"
        "    ret Box { value: value }\n"
        "  }\n\n"
        "  pub fn get(self: &Box<T>) -> &T {\n"
        "    ret &self.value\n"
        "  }\n"
        "}\n\n"
        "pub fn unwrap<T>(b: Box<T>) -> T {\n  ret b.value\n}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join("dep/out/dep.summary")));
  }
#if !BUILD_FLAG(IS_OS_ASMJS)
  {
    PipelineContext ctx{i18n::Language::EnUs};
    ctx.backend = codegen::Backend::Llvm;
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::Object);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join("dep/out/dep.o")));
  }
#endif  // !BUILD_FLAG(IS_OS_ASMJS)
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  const bool app_written = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\npath = \"main.al\"\n",
      {{"main.al",
        "use dep::Box\nuse dep::unwrap\n\nfn main() -> i32 {\n"
        "  b := Box::<i32>::wrap(21)\n"
        "  if *b.get() != 21 {\n"
        "    ret 1\n"
        "  }\n"
        "  ret unwrap(b) - 21\n}\n"}});
  CHECK(app_written);
  if (!app_written) {
    return;
  }
#if !BUILD_FLAG(IS_OS_ASMJS)
  {
    PipelineContext ctx{i18n::Language::EnUs};
    ctx.backend = codegen::Backend::Llvm;
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("proj"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::Executable);
    CHECK(built.is_ok());
    CHECK(!ctx.bag.has_errors());
    const std::string exe =
        dir.join("proj/out/app" + std::string(exe_suffix()));
    CHECK(io::is_file(exe));
    base::Result<i32, SpawnError> ran = run_command({exe});
    CHECK(ran.is_ok());
    if (ran.is_ok()) {
      CHECK(std::move(ran).unwrap() == 0);
    }
  }
#endif  // !BUILD_FLAG(IS_OS_ASMJS)
}

TEST_CASE("A dependency a summary answers for is not read") {
  // A library's build leaves its surface beside the artifact; a
  // dependent checks against it, and the library's own sources can be
  // gone when it does (ADR-0078).
  io::TempDir dir = io::TempDir::create_unique("alcy_summary_dep_test_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al",
        "pub struct Pair {\n  pub left: i32,\n  pub right: i32,\n}\n\n"
        "pub struct Line {\n  pub a: Pair,\n  pub b: Pair,\n}\n\n"
        "impl Pair {\n"
        "  pub fn new(left: i32, right: i32) -> Pair {\n"
        "    ret Pair { left: left, right: right }\n"
        "  }\n\n"
        "  pub fn total(self: &Pair) -> i32 {\n"
        "    ret self.left + self.right\n"
        "  }\n"
        "}\n\n"
        "impl Line {\n"
        "  pub fn total(self: &Line) -> i32 {\n"
        "    ret self.a.left + self.b.left\n"
        "  }\n"
        "}\n\n"
        "pub fn double(x: i32) -> i32 {\n  ret x + x\n}\n\n"
        "pub fn sum(p: Pair) -> i32 {\n  ret p.left + p.right\n}\n\n"
        "pub fn make(a: i32, b: i32) -> Pair {\n"
        "  ret Pair { left: a, right: b }\n}\n\n"
        "pub fn left_of(p: &Pair) -> i32 {\n  ret p.left\n}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  // Build the library once, so the summary is written.
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join("dep/out/dep.summary")));
  }
#if !BUILD_FLAG(IS_OS_ASMJS)
  // The object a dependent links comes from the same build.
  {
    PipelineContext ctx{i18n::Language::EnUs};
    ctx.backend = codegen::Backend::Llvm;
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::Object);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join("dep/out/dep.o")));
  }
#endif  // !BUILD_FLAG(IS_OS_ASMJS)
  const bool app_written = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\npath = \"main.al\"\n",
      {{"main.al",
        "use dep::Line\nuse dep::Pair\nuse dep::double\nuse dep::left_of\n"
        "use dep::make\nuse dep::sum\n\nfn main() -> i32 {\n"
        "  p := Pair::new(20, 1)\n"
        "  l := Line { a: p, b: p }\n"
        "  ret double(sum(l.a) + left_of(&l.a) + p.total() - 62)\n}\n"}});
  CHECK(app_written);
  if (!app_written) {
    return;
  }
  // The library's source is what a dependent would otherwise read.
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  {
    PipelineContext ctx{i18n::Language::EnUs};
    const base::Result<CheckOutcome, diag::Reported> checked =
        check_proj(ctx, dir);
    CHECK(checked.is_ok());
    CHECK_FALSE(ctx.bag.has_errors());
  }
  // The signature came from the summary: a second argument is refused.
  CHECK(dir.write_file(
      "proj/main.al",
      "use dep::double\n\nfn main() -> i32 {\n  ret double(21, 1) - 42\n}\n"));
  {
    PipelineContext ctx{i18n::Language::EnUs};
    const base::Result<CheckOutcome, diag::Reported> checked =
        check_proj(ctx, dir);
    const bool refused = checked.is_err() || ctx.bag.has_errors();
    CHECK(refused);
  }
  // Put the right call back: the dependent links the library's object,
  // named by the symbol the summary carried, and it spells the
  // library's struct from the surface (ADR-0078).
  CHECK(dir.write_file(
      "proj/main.al",
      "use dep::Line\nuse dep::Pair\nuse dep::double\nuse dep::left_of\n"
      "use dep::make\nuse dep::sum\n\nfn main() -> i32 {\n"
      "  p := Pair::new(20, 1)\n"
      "  l := Line { a: p, b: p }\n"
      "  ret double(sum(l.a) + left_of(&l.a) + p.total() + l.total() - 102)\n"
      "}\n"));
#if !BUILD_FLAG(IS_OS_ASMJS)
  {
    PipelineContext ctx{i18n::Language::EnUs};
    ctx.backend = codegen::Backend::Llvm;
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("proj"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::Executable);
    CHECK(built.is_ok());
    CHECK(!ctx.bag.has_errors());
    const std::string exe =
        dir.join("proj/out/app" + std::string(exe_suffix()));
    CHECK(io::is_file(exe));
    // The linked program runs the dependency's code.
    base::Result<i32, SpawnError> ran = run_command({exe});
    CHECK(ran.is_ok());
    if (ran.is_ok()) {
      CHECK(std::move(ran).unwrap() == 0);
    }
  }
#endif  // !BUILD_FLAG(IS_OS_ASMJS)
}

TEST_CASE("A summary import carries what a carried body reaches") {
  // A dependent holds the generic bodies a summary carries and the
  // concrete code those bodies call, not the library's whole lowered
  // storage (ADR-0079).
  io::TempDir dir = io::TempDir::create_unique("alcy_summary_reach_test_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al",
        "fn tag() -> i32 {\n  ret 7\n}\n\n"
        "fn sink() -> i32 {\n  ret 3\n}\n\n"
        "pub fn identity<T>(x: T) -> T {\n"
        "  _ := tag()\n"
        "  ret x\n"
        "}\n\n"
        "pub fn double(x: i32) -> i32 {\n  ret x + x\n}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  // Build the library once, so the summary and the artifact exist.
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
  }
  // The library's sources are gone, so the summary answers for it.
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  const bool app_written = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\npath = \"main.al\"\n",
      {{"main.al",
        "use dep::identity\n\nfn main() -> i32 {\n"
        "  ret identity(21i32) - 21\n}\n"}});
  CHECK(app_written);
  if (!app_written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("proj"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
  }
  const std::optional<std::string> bytes =
      io::read_file(dir.join("proj/out/app.irb"));
  CHECK(bytes.has_value());
  if (!bytes.has_value() || bytes->size() < 4) {
    return;
  }
  const std::span<const u8> written_ir(
      reinterpret_cast<const u8*>(bytes->data()), bytes->size());
  const base::Result<ir::LoadedIr, ir::IrLoadError> loaded =
      ir::deserialize(written_ir);
  CHECK(loaded.is_ok());
  if (loaded.is_err()) {
    return;
  }
  const ir::LoadedIr& package = loaded.value();
  // The carried body and the concrete helper it calls cross; the
  // library's unreached functions - a private helper no carried body
  // calls and an unused surface function - stay in the library's own
  // storage.
  bool inherited_body = false;
  bool inherited_helper = false;
  bool inherited_unreached = false;
  for (const ir::FunctionIdx idx : package.storage->functions().idx_range()) {
    const ir::FunctionMeta& meta = package.storage->functions()[idx].meta;
    const std::string_view name = package.strings->get(meta.name);
    if (name == "identity") {
      bool mentions =
          package.storage->types()[meta.return_type].tag == ir::TypeTag::Param;
      for (const ir::TypeIdx param : meta.param_types) {
        mentions = mentions ||
                   package.storage->types()[param].tag == ir::TypeTag::Param;
      }
      inherited_body = inherited_body || mentions;
    }
    if (name == "tag") {
      inherited_helper = true;
    }
    if (name == "sink" || name == "double") {
      inherited_unreached = true;
    }
  }
  CHECK(inherited_body);
  CHECK(inherited_helper);
  CHECK_FALSE(inherited_unreached);
  for (const ir::ExternalFunction& external :
       package.storage->external_functions()) {
    CHECK(package.strings->get(external.meta.name) != "double");
  }
}

TEST_CASE("A package's storage merges once for all of its modules") {
  // Every module of a summary dependency shares one artifact storage,
  // so the merge copies a reached entry once, however many modules name
  // it: one module's generic calling another's crosses as one body
  // (ADR-0079).
  io::TempDir dir = io::TempDir::create_unique("alcy_summary_shared_test_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\", \"extra\"]\nexport = [\"extra\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al", "pub fn identity<T>(x: T) -> T {\n  ret x\n}\n"},
       {"extra.al",
        "use package::identity\n\n"
        "pub fn wrap<T>(x: T) -> T {\n  ret identity(x)\n}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
  }
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  CHECK(io::remove_file(dir.join("dep/extra.al")));
  const bool app_written = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\npath = \"main.al\"\n",
      {{"main.al",
        "use dep::extra::wrap\n\nfn main() -> i32 {\n"
        "  ret wrap(21i32) - 21\n}\n"}});
  CHECK(app_written);
  if (!app_written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("proj"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
  }
  const std::optional<std::string> bytes =
      io::read_file(dir.join("proj/out/app.irb"));
  CHECK(bytes.has_value());
  if (!bytes.has_value() || bytes->size() < 4) {
    return;
  }
  const std::span<const u8> written_ir(
      reinterpret_cast<const u8*>(bytes->data()), bytes->size());
  const base::Result<ir::LoadedIr, ir::IrLoadError> loaded =
      ir::deserialize(written_ir);
  CHECK(loaded.is_ok());
  if (loaded.is_err()) {
    return;
  }
  const ir::LoadedIr& package = loaded.value();
  usize identity_bodies = 0;
  usize wrap_bodies = 0;
  for (const ir::FunctionIdx idx : package.storage->functions().idx_range()) {
    const ir::FunctionMeta& meta = package.storage->functions()[idx].meta;
    const std::string_view name = package.strings->get(meta.name);
    if (name != "identity" && name != "wrap") {
      continue;
    }
    bool mentions =
        package.storage->types()[meta.return_type].tag == ir::TypeTag::Param;
    for (const ir::TypeIdx param : meta.param_types) {
      mentions =
          mentions || package.storage->types()[param].tag == ir::TypeTag::Param;
    }
    if (!mentions) {
      continue;
    }
    identity_bodies += name == "identity" ? 1 : 0;
    wrap_bodies += name == "wrap" ? 1 : 0;
  }
  CHECK(identity_bodies == 1);
  CHECK(wrap_bodies == 1);
}

TEST_CASE("A summary destructor the carrier did not cross is refused") {
  // A carried body's end waits for the substitution, and a type the
  // library ends through a destructor crosses as structure alone: the
  // drop cannot run in the dependent, so the build stops rather than
  // leak what the destructor owned (ADR-0085, ADR-0079).
  io::TempDir dir = io::TempDir::create_unique("alcy_summary_drop_refused_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al",
        "struct Box<T> {\n  value: T,\n}\n\n"
        "impl<T> Box<T> {\n"
        "  fn new(value: T) -> Box<T> {\n    ret Box { value: value }\n  }\n\n"
        "  fn drop(self: Box<T>) {\n  }\n"
        "}\n\n"
        "fn internal<U>(x: U) {\n}\n\n"
        "pub fn outer<T>(x: T) -> T {\n"
        "  internal(Box::<i32>::new(7))\n"
        "  ret x\n}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
  }
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  const bool app_written = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\npath = \"main.al\"\n",
      {{"main.al",
        "use dep::outer\n\nfn main() -> i32 {\n  ret outer(3) - 3\n}\n"}});
  CHECK(app_written);
  if (!app_written) {
    return;
  }
  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(dir.join("proj"));
  CHECK(root.is_ok());
  if (root.is_err()) {
    return;
  }
  const path::Path root_path = std::move(root).unwrap();
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(root_path.join("alcy.toml").as_view());
  CHECK(manifest.is_ok());
  if (manifest.is_err()) {
    return;
  }
  const base::Result<std::string, diag::Reported> built =
      build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                    "", BuildPlan{}, EmitMode::IrBinary);
  CHECK(built.is_err());
  CHECK(reports(
      ctx.bag,
      "Internal lowering error: a summary type's destructor did not cross"));
}

TEST_CASE("A carried body ends a summary type the library did not arm") {
  // The other side of the boundary: a type with no destructor crosses
  // as structure, and ending a value of it runs nothing in the
  // dependent, so the drop is a plain no-op (ADR-0085, ADR-0079).
  io::TempDir dir = io::TempDir::create_unique("alcy_summary_drop_plain_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al",
        "struct Cell<T> {\n  value: T,\n}\n\n"
        "impl<T> Cell<T> {\n"
        "  fn new(value: T) -> Cell<T> {\n    ret Cell { value: value }\n  }\n"
        "}\n\n"
        "fn internal<U>(x: U) {\n}\n\n"
        "pub fn outer<T>(x: T) -> T {\n"
        "  internal(Cell::<i32>::new(7))\n"
        "  ret x\n}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
  }
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  const bool app_written = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\npath = \"main.al\"\n",
      {{"main.al",
        "use dep::outer\n\nfn main() -> i32 {\n  ret outer(3) - 3\n}\n"}});
  CHECK(app_written);
  if (!app_written) {
    return;
  }
  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(dir.join("proj"));
  CHECK(root.is_ok());
  if (root.is_err()) {
    return;
  }
  const path::Path root_path = std::move(root).unwrap();
  base::Result<source::FileId, source::SourceError> manifest =
      ctx.sources.load(root_path.join("alcy.toml").as_view());
  CHECK(manifest.is_ok());
  if (manifest.is_err()) {
    return;
  }
  const base::Result<std::string, diag::Reported> built =
      build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                    "", BuildPlan{}, EmitMode::IrBinary);
  CHECK(built.is_ok());
  CHECK(!ctx.bag.has_errors());
}

TEST_CASE("A carried body ends a generic aggregate once per element") {
  // The substitution reaches an element through storage copies, and
  // the end still runs the destructor the declaration measured: a copy
  // of a copy resolves past both (ADR-0079, ADR-0085).
  io::TempDir dir = io::TempDir::create_unique("alcy_summary_tuple_drop_");
  const bool written = write_package(
      dir, "dep",
      "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"lib\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n\n"
      "[lib]\npath = \"lib.al\"\n",
      {{"lib.al",
        "pub fn sink_pair<T>(a: T, b: T) {\n"
        "  p := (a, b)\n"
        "  q := &p\n"
        "  ret\n"
        "}\n"}});
  CHECK(written);
  if (!written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("dep"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
  }
  CHECK(io::remove_file(dir.join("dep/lib.al")));
  const bool app_written = write_package(
      dir, "proj",
      "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
      "[modules]\ninclude = [\"main\"]\n\n"
      "[dependencies]\n\"alcy/std/core\" = {}\n"
      "dep = { path = \"../dep\" }\n\n"
      "[[bin]]\npath = \"main.al\"\n",
      {{"main.al",
        "use dep::sink_pair\n\n"
        "struct Noisy {\n"
        "  n: i32,\n"
        "}\n\n"
        "impl Noisy {\n"
        "  fn drop(self: Noisy) {\n  }\n"
        "}\n\n"
        "fn main() -> i32 {\n"
        "  sink_pair(Noisy { n: 1 }, Noisy { n: 2 })\n"
        "  ret 0\n"
        "}\n"}});
  CHECK(app_written);
  if (!app_written) {
    return;
  }
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<path::Path, path::PathError> root =
        path::Path::from_native(dir.join("proj"));
    CHECK(root.is_ok());
    if (root.is_err()) {
      return;
    }
    const path::Path root_path = std::move(root).unwrap();
    base::Result<source::FileId, source::SourceError> manifest =
        ctx.sources.load(root_path.join("alcy.toml").as_view());
    CHECK(manifest.is_ok());
    if (manifest.is_err()) {
      return;
    }
    const base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", BuildPlan{}, EmitMode::IrBinary);
    CHECK(built.is_ok());
    CHECK(!ctx.bag.has_errors());
  }
  const std::optional<std::string> bytes =
      io::read_file(dir.join("proj/out/app.irb"));
  CHECK(bytes.has_value());
  if (!bytes.has_value() || bytes->size() < 4) {
    return;
  }
  const std::span<const u8> written_ir(
      reinterpret_cast<const u8*>(bytes->data()), bytes->size());
  const base::Result<ir::LoadedIr, ir::IrLoadError> loaded =
      ir::deserialize(written_ir);
  CHECK(loaded.is_ok());
  if (loaded.is_err()) {
    return;
  }
  const ir::LoadedIr& package = loaded.value();
  // The aggregate's drop walk calls the destructor once per element;
  // the clone reaches the walk, and the walk's calls are the count.
  usize element_drops = 0;
  for (const ir::FunctionIdx idx : package.storage->functions().idx_range()) {
    const ir::Function& function = package.storage->functions()[idx];
    const std::string_view name = package.strings->get(function.meta.name);
    if (!name.starts_with("drop$")) {
      continue;
    }
    for (const ir::BlockIdx block_idx : function.blocks) {
      const ir::Block& block = package.storage->blocks()[block_idx];
      for (const ir::InstructionIdx iidx : block.instrs) {
        const ir::Instruction& instr = package.storage->instrs()[iidx];
        if (instr.op != ir::Opcode::Call || instr.operands.empty()) {
          continue;
        }
        const ir::Operand& callee =
            package.storage->operands()[instr.operands.head()];
        if (!callee.is<ir::FunctionIdx>()) {
          continue;
        }
        const ir::FunctionMeta& meta =
            package.storage->functions()[callee.as_function()].meta;
        if (package.strings->get(meta.name) == "drop") {
          ++element_drops;
        }
      }
    }
  }
  CHECK(element_drops == 2);
}

}  // namespace pipeline
