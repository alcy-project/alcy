// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/check.h"

#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

namespace {

// A package with both halves over one module selection. `package::`
// resolves at a tree's root, so the binary's tree and the library's
// tree answer its calls differently: a problem rooted in one is
// invisible from the other.
constexpr std::string_view SHARED_MANIFEST =
    "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
    "[modules]\ninclude = [\"main\", \"lib\"]\n\n"
    "[[bin]]\npath = \"main.al\"\n\n"
    "[lib]\npath = \"lib.al\"\n";

base::Result<io::TempDir, diag::Reported> make_package(std::string_view main,
                                                       std::string_view lib) {
  io::TempDir dir = io::TempDir::create_unique("alcy_check_test_");
  const bool setup = dir.write_file("proj/alcy.toml", SHARED_MANIFEST) &&
                     dir.write_file("proj/main.al", main) &&
                     dir.write_file("proj/lib.al", lib);
  if (!setup) {
    return base::make_err(diag::Reported{});
  }
  return base::make_ok(std::move(dir));
}

base::Result<CheckOutcome, diag::Reported> check_dir(PipelineContext& ctx,
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

}  // namespace

TEST_CASE("Check sees an error only the library's tree carries") {
  // The binary's root defines `helper`, the library's root calls it
  // through `package::`. In the library's tree the root is lib.al, so
  // the call is unresolved; checking the binary's tree alone would miss
  // it, which is what a build then reported.
  base::Result<io::TempDir, diag::Reported> made = make_package(
      "fn helper() -> i32 {\n  ret 1\n}\n\n"
      "fn main() -> i32 {\n  ret 0\n}\n",
      "pub fn double(x: i32) -> i32 {\n  ret package::helper() + x\n}\n");
  CHECK(made.is_ok());
  if (made.is_err()) {
    return;
  }
  const io::TempDir dir = std::move(made).unwrap();

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_dir(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  bool named = false;
  ctx.bag.for_each([&](const diag::Diagnostic& diagnostic) {
    named = named || diagnostic.message == "Unresolved value 'helper'";
  });
  CHECK(named);
}

TEST_CASE("Check reports one warning for a problem both trees share") {
  // Both trees lower main.al - as the root once and as a module once -
  // so the unreachable statement is found twice. A reader sees it once.
  base::Result<io::TempDir, diag::Reported> made = make_package(
      "fn tick() {}\n\n"
      "fn main() {\n  ret\n  tick()\n}\n",
      "pub fn double(x: i32) -> i32 {\n  ret x + x\n}\n");
  CHECK(made.is_ok());
  if (made.is_err()) {
    return;
  }
  const io::TempDir dir = std::move(made).unwrap();

  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<CheckOutcome, diag::Reported> outcome = check_dir(ctx, dir);
  CHECK(outcome.is_ok());
  CHECK(!ctx.bag.has_errors());
  CHECK(ctx.bag.warning_count() == 1);
}

TEST_CASE("Check reports an uncaptured use") {
  // A capture list is checked against the declared modes; a use of
  // an outer local that the list does not name reports.
  base::Result<io::TempDir, diag::Reported> made = make_package(
      "fn main() -> i32 {\n  t := 1\n  f := (a: i32) -> a + t\n  ret f(0)\n}\n",
      "pub fn double(x: i32) -> i32 {\n  ret x + x\n}\n");
  CHECK(made.is_ok());
  if (made.is_err()) {
    return;
  }
  const io::TempDir dir = std::move(made).unwrap();

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_dir(ctx, dir).is_err());
  CHECK(ctx.bag.has_errors());
  bool named = false;
  ctx.bag.for_each([&](const diag::Diagnostic& diagnostic) {
    named =
        named || diagnostic.message ==
                     "Use of 't' is not captured; add it to the capture list";
  });
  CHECK(named);
}

TEST_CASE("Check runs a closure that captures") {
  base::Result<io::TempDir, diag::Reported> made = make_package(
      "fn main() -> i32 {\n  t := 1\n  f := [&t] (a: i32) -> a + t\n  ret "
      "f(0)\n}\n",
      "pub fn double(x: i32) -> i32 {\n  ret x + x\n}\n");
  CHECK(made.is_ok());
  if (made.is_err()) {
    return;
  }
  const io::TempDir dir = std::move(made).unwrap();

  PipelineContext ctx{i18n::Language::EnUs};
  CHECK(check_dir(ctx, dir).is_ok());
  CHECK(!ctx.bag.has_errors());
}

}  // namespace pipeline
