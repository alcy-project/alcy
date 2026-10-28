// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/cache.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "pipeline/artifact.h"
#include "pipeline/build.h"
#include "pipeline/build_plan.h"
#include "pipeline/emit_mode.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

namespace {

constexpr std::string_view MANIFEST =
    "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[[bin]]\nname = \"app\"\npath = \"main.al\"\n";
constexpr std::string_view PROGRAM = "fn main() -> i32 {\n  ret 0\n}\n";
constexpr std::string_view PROGRAM_CHANGED = "fn main() -> i32 {\n  ret 1\n}\n";

// Two targets in one package, with names of their own so their default
// outputs do not collide.
constexpr std::string_view LIB_BIN_MANIFEST =
    "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[modules]\ninclude = [\"main\", \"lib\"]\n\n"
    "[dependencies]\n\"alcy/std/core\" = {}\n\n"
    "[lib]\nname = \"math\"\npath = \"lib.al\"\n\n"
    "[[bin]]\nname = \"app\"\npath = \"main.al\"\n";

constexpr std::string_view DEP_MANIFEST =
    "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[modules]\ninclude = [\"lib\"]\n\n"
    "[dependencies]\n\"alcy/std/core\" = {}\n\n"
    "[lib]\npath = \"lib.al\"\n";

constexpr std::string_view APP_WITH_DEP_MANIFEST =
    "[package]\nname = \"app\"\nversion = \"0.1.0\"\nlicense = \"\"\n\n"
    "[modules]\ninclude = [\"main\"]\n\n"
    "[dependencies]\n\"alcy/std/core\" = {}\n"
    "dep = { path = \"../dep\" }\n\n"
    "[[bin]]\nname = \"app\"\npath = \"main.al\"\n";

struct Built {
  bool ok = false;
  CacheOutcome outcome = CacheOutcome::Off;
  std::string bytes;
};

// Builds one package in `dir` with a fresh context, the way a second
// invocation would: nothing carries over except the output directory.
Built build_in(io::TempDir& dir, std::string_view rel, bool no_cache) {
  Built built;
  PipelineContext ctx{i18n::Language::EnUs};
  ctx.no_cache = no_cache;
  base::Result<ManifestProbe, diag::Reported> probe =
      require_package_manifest(ctx, dir.join(rel), false);
  if (probe.is_err()) {
    return built;
  }
  ManifestProbe found = std::move(probe).unwrap();
  base::Result<std::string, diag::Reported> res =
      build_package(ctx, found.root, found.manifest, found.manifest_name, "",
                    BuildPlan{}, EmitMode::IrBinary);
  if (res.is_err() || ctx.bag.has_errors()) {
    return built;
  }
  built.ok = true;
  built.outcome = ctx.cache_outcome;
  built.bytes = io::read_file(std::move(res).unwrap());
  return built;
}

Built build_once(io::TempDir& dir, bool no_cache) {
  return build_in(dir, "proj", no_cache);
}

bool write_package(io::TempDir& dir, std::string_view program) {
  return dir.write_file("proj/alcy.toml", MANIFEST) &&
         dir.write_file("proj/main.al", program);
}

TEST_CASE("A run's lowering takes the entry a build stored") {
  io::TempDir dir = io::TempDir::create_unique("alcy_cache_run_test_");
  CHECK(write_package(dir, PROGRAM));
  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<ManifestProbe, diag::Reported> probe =
      require_package_manifest(ctx, dir.join("proj"), false);
  CHECK(probe.is_ok());
  if (probe.is_err()) {
    return;
  }
  ManifestProbe found = std::move(probe).unwrap();
  // The build lowers the executable and stores it.
  CHECK(build_package(ctx, found.root, found.manifest, found.manifest_name, "",
                      BuildPlan{}, EmitMode::Executable)
            .is_ok());
  CHECK(ctx.cache_outcome == CacheOutcome::Miss);

  // The front half a run walks is the one the build walked: the same
  // target in the same mode, so a run after a build rebuilds nothing. A
  // run names a scratch executable instead of an output path, which the
  // key does not read.
  base::Result<std::vector<Artifact>, diag::Reported> targets =
      resolve_artifacts(ctx, found.root, found.manifest, found.manifest_name,
                        ArtifactScope::BinOnly);
  CHECK(targets.is_ok());
  if (targets.is_err()) {
    return;
  }
  std::vector<Artifact> resolved = std::move(targets).unwrap();
  CHECK(resolved.size() == 1);
  if (resolved.empty()) {
    return;
  }
  ctx.cache_outcome = CacheOutcome::Off;
  const PlannedArtifact planned{&resolved.front(), EmitMode::Executable,
                                "scratch/app"};
  base::Result<lowering::LoweredPackage, diag::Reported> lowered =
      lower_target(ctx, planned, found.root, found.manifest);
  CHECK(lowered.is_ok());
  CHECK(!ctx.bag.has_errors());
  CHECK(ctx.cache_outcome == CacheOutcome::Hit);
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

TEST_CASE("The targets of one package do not answer from one entry") {
  // Two targets of one package read the same sources; the key has to
  // tell them apart, or the second lowers from the first's tree.
  io::TempDir dir = io::TempDir::create_unique("alcy_cache_target_test_");
  const bool written =
      dir.write_file("proj/alcy.toml", LIB_BIN_MANIFEST) &&
      dir.write_file("proj/main.al", PROGRAM) &&
      dir.write_file("proj/lib.al",
                     "pub fn double(x: i32) -> i32 {\n  ret x + x\n}\n");
  CHECK(written);
  if (!written) {
    return;
  }
  const Built built = build_once(dir, false);
  CHECK(built.ok);
  // The library lowered from its own tree, so it carries a surface; a
  // library answered from the binary's entry would carry none.
  CHECK(io::is_file(dir.join("proj/out/math.summary")));
  // And the two artifacts are not one file written twice.
  CHECK(built.bytes != io::read_file(dir.join("proj/out/math.irb")));
}

TEST_CASE("A dependency's summary steers the dependent's entry") {
  // A dependent resolves against the summary, not the dependency's
  // sources, so a rebuilt dependency changes no file the dependent
  // read. Its key has to carry the summary itself.
  io::TempDir dir = io::TempDir::create_unique("alcy_cache_dep_test_");
  const bool written =
      dir.write_file("dep/alcy.toml", DEP_MANIFEST) &&
      dir.write_file("dep/lib.al", "pub fn answer() -> i32 {\n  ret 7\n}\n") &&
      dir.write_file("proj/alcy.toml", APP_WITH_DEP_MANIFEST) &&
      dir.write_file("proj/main.al",
                     "use dep::answer\n\nfn main() -> i32 {\n"
                     "  ret answer() - 7\n}\n");
  CHECK(written);
  if (!written) {
    return;
  }
  const Built dep = build_in(dir, "dep", false);
  CHECK(dep.ok);
  const Built first = build_in(dir, "proj", false);
  CHECK(first.ok);
  CHECK(first.outcome == CacheOutcome::Miss);
  // Rebuild the dependency from a changed body: a fresh summary, and
  // no file the dependent read is different.
  CHECK(dir.write_file("dep/lib.al", "pub fn answer() -> i32 {\n  ret 8\n}\n"));
  const Built dep_again = build_in(dir, "dep", false);
  CHECK(dep_again.ok);
  CHECK(dep_again.outcome == CacheOutcome::Miss);
  const Built second = build_in(dir, "proj", false);
  CHECK(second.ok);
  CHECK(second.outcome == CacheOutcome::Miss);
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
