// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/modules.h"

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

namespace {

constexpr std::string_view BASE_MANIFEST =
    "[package]\n"
    "name = \"demo\"\n"
    "version = \"0.1.0\"\n"
    "\n"
    "[[bin]]\n"
    "name = \"demo\"\n"
    "path = \"main.al\"\n";

pkg::PackageManifest parse_ok(std::string_view text, PipelineContext& ctx) {
  base::Result<pkg::PackageManifest, diag::Reported> result =
      pkg::parse_manifest(text, "alcy.toml", source::UNKNOWN_FILE, ctx.bag,
                          ctx.arena);
  CHECK(result.is_ok());
  if (result.is_err()) {
    return pkg::PackageManifest{};
  }
  return std::move(result).unwrap();
}

// The root and the loaded files one case works on. Files are loaded under
// canonical paths, which is what discovery hands the selection: a name
// with a native separator is not the same string as the candidate it
// should match.
struct Loaded {
  path::Path root;
  std::vector<source::FileId> files;
};

Loaded load(PipelineContext& ctx,
            const io::TempDir& dir,
            std::initializer_list<std::string_view> rels) {
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(dir.path());
  // A temp directory's path is representable, and the case just wrote into
  // it, so there is nothing to report here.
  CHECK(root.is_ok());
  Loaded loaded{std::move(root).unwrap(), {}};
  for (std::string_view rel : rels) {
    const path::Path path = loaded.root.join(rel);
    base::Result<source::FileId, source::SourceError> id =
        ctx.sources.load(path.as_view());
    CHECK(id.is_ok());
    if (id.is_err()) {
      continue;
    }
    loaded.files.push_back(std::move(id).unwrap());
  }
  return loaded;
}

}  // namespace

TEST_CASE("Modules resolve explicit entries to files") {
  io::TempDir dir = io::TempDir::create_unique("alcy_modules_explicit_test_");
  const bool setup =
      dir.write_file("main.al", "fn main() {}\n") &&
      dir.write_file("util.al", "fn double(x: i32) -> i32 {\n  ret x\n}\n") &&
      dir.write_file("extra.al", "fn unused() {}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  PipelineContext ctx{i18n::Language::EnUs};
  const pkg::PackageManifest manifest =
      parse_ok(std::string(BASE_MANIFEST) +
                   "\n[modules]\n"
                   "include = [\"main\", \"util\"]\n",
               ctx);
  const Loaded loaded = load(ctx, dir, {"main.al", "util.al", "extra.al"});
  base::Result<std::vector<analyzer::ModuleInput>, diag::Reported> resolved =
      select_modules(ctx, manifest, loaded.root, loaded.files);
  CHECK(resolved.is_ok());
  if (resolved.is_err()) {
    return;
  }
  const std::vector<analyzer::ModuleInput> selected =
      std::move(resolved).unwrap();
  CHECK(selected.size() == 2);
  if (selected.size() != 2) {
    return;
  }
  CHECK(selected[0].name == "main");
  CHECK(selected[1].name == "util");
  CHECK(!ctx.bag.has_errors());
  // extra.al is discovered but unselected.
  CHECK(ctx.bag.warning_count() > 0);
}

TEST_CASE("Modules resolve wildcards by relative path") {
  io::TempDir dir = io::TempDir::create_unique("alcy_modules_wildcard_test_");
  const bool setup = dir.write_file("main.al", "fn main() {}\n") &&
                     dir.make_dir("io") &&
                     dir.write_file("io/util.al", "fn helper() {}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  PipelineContext ctx{i18n::Language::EnUs};
  const pkg::PackageManifest manifest = parse_ok(BASE_MANIFEST, ctx);
  CHECK(manifest.modules.wildcard);
  const Loaded loaded = load(ctx, dir, {"main.al", "io/util.al"});
  base::Result<std::vector<analyzer::ModuleInput>, diag::Reported> resolved =
      select_modules(ctx, manifest, loaded.root, loaded.files);
  CHECK(resolved.is_ok());
  if (resolved.is_err()) {
    return;
  }
  const std::vector<analyzer::ModuleInput> selected =
      std::move(resolved).unwrap();
  CHECK(selected.size() == 2);
  if (selected.size() != 2) {
    return;
  }
  CHECK(selected[0].name == "main");
  CHECK(selected[1].name == "io/util");
}

TEST_CASE("Modules reject missing include files") {
  io::TempDir dir = io::TempDir::create_unique("alcy_modules_missing_test_");
  const bool setup = dir.write_file("main.al", "fn main() {}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  PipelineContext ctx{i18n::Language::EnUs};
  const pkg::PackageManifest manifest =
      parse_ok(std::string(BASE_MANIFEST) +
                   "\n[modules]\n"
                   "include = [\"main\", \"ghost\"]\n",
               ctx);
  const Loaded loaded = load(ctx, dir, {"main.al"});
  base::Result<std::vector<analyzer::ModuleInput>, diag::Reported> resolved =
      select_modules(ctx, manifest, loaded.root, loaded.files);
  CHECK(resolved.is_err());
  CHECK(ctx.bag.has_errors());
  const diag::Diagnostic* const only = ctx.bag.at(0);
  CHECK(only != nullptr);
  if (only != nullptr) {
    CHECK(only->code ==
          diag::Code{diag::Stage::Pipeline,
                     static_cast<u8>(DiagCode::InvalidModuleSelection)});
  }
}

// A file that is not a source file is skipped rather than named: the
// wildcard walks what discovery found, and only `.al` became a module.
TEST_CASE("Modules ignore files that are not sources") {
  io::TempDir dir = io::TempDir::create_unique("alcy_modules_non_source_test_");
  const bool setup = dir.write_file("main.al", "fn main() {}\n") &&
                     dir.write_file("notes.txt", "not source\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  PipelineContext ctx{i18n::Language::EnUs};
  const pkg::PackageManifest manifest = parse_ok(BASE_MANIFEST, ctx);
  const Loaded loaded = load(ctx, dir, {"main.al", "notes.txt"});
  base::Result<std::vector<analyzer::ModuleInput>, diag::Reported> resolved =
      select_modules(ctx, manifest, loaded.root, loaded.files);
  CHECK(resolved.is_ok());
  if (resolved.is_err()) {
    return;
  }
  const std::vector<analyzer::ModuleInput> selected =
      std::move(resolved).unwrap();
  CHECK(selected.size() == 1);
}

}  // namespace pipeline
