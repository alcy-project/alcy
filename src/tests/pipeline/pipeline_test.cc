// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "pipeline/pipeline.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "pkg/resolve.h"
#include "source/source.h"
#include "tests/util/test_fs.h"

namespace pipeline {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena};
  source::SourceManager sources;

  Fixture() { arena.reserve(1u << 20); }
};

}  // namespace

TEST_CASE("Discover finds nested sources in sorted order") {
  test_fs::TempDir dir("alcy_pipeline_test");
  const bool setup = dir.write_file("src/main.al", "fn main() {}\n") &&
                     dir.write_file("src/util.al", "") &&
                     dir.write_file("README.md", "not source\n") &&
                     dir.write_file("a.al", "");
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  diag::Fallible<DiscoveredSources> result =
      discover_sources(dir.path(), f.sources, f.bag);
  CHECK(result.is_ok());
  if (!result.is_ok()) {
    return;
  }
  const DiscoveredSources discovered = std::move(result).unwrap();
  CHECK(!f.bag.has_errors());
  CHECK(discovered.files.size() == 3);
  if (discovered.files.size() != 3) {
    return;
  }
  // Sorted: a.al, src/main.al, src/util.al.
  CHECK(f.sources.name(discovered.files[0]) == dir.join("a.al"));
  CHECK(f.sources.name(discovered.files[1]) == dir.join("src/main.al"));
  CHECK(f.sources.name(discovered.files[2]) == dir.join("src/util.al"));
}

TEST_CASE("Discover reports missing directories") {
  Fixture f;
  CHECK(discover_sources("/nonexistent-dir-xyz", f.sources, f.bag).is_err());
  CHECK(f.bag.has_errors());
}

TEST_CASE("Compile project loads every package") {
  test_fs::TempDir dir("alcy_pipeline_project_test");
  const bool setup =
      dir.write_file("root/alcy.toml",
                     "[package]\nname = \"root\"\nversion = \"0.1.0\"\n"
                     "[dependencies]\nlib = { path = \"lib\" }\n") &&
      dir.write_file("root/src/main.al", "fn main() {}\n") &&
      dir.write_file("root/lib/alcy.toml",
                     "[package]\nname = \"lib\"\nversion = \"0.1.0\"\n") &&
      dir.write_file("root/lib/src/lib.al", "");
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  diag::Fallible<std::vector<pkg::ResolvedPackage>> resolved =
      pkg::resolve_package(dir.join("root"), f.sources, f.arena, f.bag);
  CHECK(resolved.is_ok());
  if (!resolved.is_ok()) {
    return;
  }

  diag::Fallible<ProjectBuild> built =
      compile_project(std::move(resolved).unwrap(), f.sources, f.bag);
  CHECK(built.is_ok());
  if (!built.is_ok()) {
    return;
  }
  const ProjectBuild build = std::move(built).unwrap();
  CHECK(!f.bag.has_errors());
  CHECK(build.packages == 2);
  CHECK(build.files_loaded == 2);
}

TEST_CASE("Fetch adapter feeds the renderer") {
  test_fs::TempDir dir("alcy_pipeline_fetch_test");
  const bool setup = dir.write_file("b.al", "let y = 2;\n");
  CHECK(setup);
  if (!setup) {
    return;
  }

  Fixture f;
  base::Result<source::FileId, source::SourceError> loaded =
      f.sources.load(dir.join("b.al"));
  CHECK(loaded.is_ok());
  if (!loaded.is_ok()) {
    return;
  }
  const source::FileId id = std::move(loaded).unwrap();

  const u32 index =
      f.bag.emit(diag::Severity::Error, 1, diag::Span{id, 4, 1}, "bad token");
  const diag::Diagnostic& diag = f.bag.at(index);

  fmt::memory_buffer out;
  diag::render(diag, out, {}, fetch_source, &f.sources);
  const std::string rendered(out.data(), out.size());
  CHECK(rendered.find(":1:5\n") != std::string::npos);
  CHECK(rendered.find("let y = 2;") != std::string::npos);

  diag::SourceText unknown = fetch_source(999, &f.sources);
  CHECK(unknown.bytes.empty());
}

}  // namespace pipeline
