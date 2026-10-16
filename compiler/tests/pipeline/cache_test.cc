// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/cache.h"

#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "pipeline/build.h"
#include "pipeline/emit_mode.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/profile.h"
#include "pipeline/target.h"

namespace pipeline {

namespace {

constexpr std::string_view MANIFEST =
    "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[[bin]]\nname = \"app\"\npath = \"main.al\"\n";
constexpr std::string_view PROGRAM = "fn main() -> i32 {\n  ret 0\n}\n";
constexpr std::string_view PROGRAM_CHANGED = "fn main() -> i32 {\n  ret 1\n}\n";

struct Built {
  bool ok = false;
  CacheOutcome outcome = CacheOutcome::Off;
  std::string bytes;
};

// Builds the package in `dir` with a fresh context, the way a second
// invocation would: nothing carries over except the output directory.
Built build_once(io::TempDir& dir, bool no_cache) {
  Built built;
  PipelineContext ctx{i18n::Language::EnUs};
  ctx.no_cache = no_cache;
  base::Result<ManifestProbe, diag::Reported> probe =
      require_package_manifest(ctx, dir.join("proj"), false);
  if (probe.is_err()) {
    return built;
  }
  ManifestProbe found = std::move(probe).unwrap();
  base::Result<std::string, diag::Reported> res =
      build_package(ctx, found.root, found.manifest, found.manifest_name, "",
                    BuildProfile{}, LinkOptions{}, EmitMode::IrBinary);
  if (res.is_err() || ctx.bag.has_errors()) {
    return built;
  }
  built.ok = true;
  built.outcome = ctx.cache_outcome;
  built.bytes = io::read_file(std::move(res).unwrap());
  return built;
}

bool write_package(io::TempDir& dir, std::string_view program) {
  return dir.write_file("proj/alcy.toml", MANIFEST) &&
         dir.write_file("proj/main.al", program);
}

TEST_CASE("A second build hits the cache and answers the same bytes") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cache_hit_test_");
  CHECK(write_package(dir, PROGRAM));

  const Built first = build_once(dir, false);
  CHECK(first.ok);
  CHECK(first.outcome == CacheOutcome::Miss);
  CHECK(!first.bytes.empty());

  const Built second = build_once(dir, false);
  CHECK(second.ok);
  CHECK(second.outcome == CacheOutcome::Hit);
  CHECK(second.bytes == first.bytes);
}

TEST_CASE("A changed source misses the cache again") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cache_miss_test_");
  CHECK(write_package(dir, PROGRAM));

  const Built first = build_once(dir, false);
  CHECK(first.ok);
  CHECK(first.outcome == CacheOutcome::Miss);

  CHECK(write_package(dir, PROGRAM_CHANGED));
  const Built second = build_once(dir, false);
  CHECK(second.ok);
  CHECK(second.outcome == CacheOutcome::Miss);
  CHECK(second.bytes != first.bytes);
}

TEST_CASE("A no-cache build consults nothing and stores nothing") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cache_off_test_");
  CHECK(write_package(dir, PROGRAM));

  const Built first = build_once(dir, true);
  CHECK(first.ok);
  CHECK(first.outcome == CacheOutcome::Off);

  const Built second = build_once(dir, true);
  CHECK(second.ok);
  CHECK(second.outcome == CacheOutcome::Off);
}

}  // namespace

}  // namespace pipeline
