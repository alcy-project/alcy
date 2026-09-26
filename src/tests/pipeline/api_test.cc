// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <span>
#include <string>
#include <utility>

#include "analyzer/resolve.h"
#include "config/build_config.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_stage.h"
#include "source/source.h"

namespace pipeline {

#if !BUILD_FLAG(IS_OS_ASMJS)
TEST_CASE("Pipeline stages the standard library prelude") {
  PipelineContext ctx;
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> prelude =
      std_prelude(ctx);
  CHECK(prelude.is_ok());
  CHECK(!ctx.bag.has_errors());
  if (prelude.is_err()) {
    return;
  }
  const std::span<const analyzer::ModuleInput> inputs =
      std::move(prelude).unwrap();
  CHECK(!inputs.empty());
  if (inputs.empty()) {
    return;
  }
  CHECK(inputs[0].name == "core");
  CHECK(inputs[0].id != source::UNKNOWN_FILE);
}
TEST_CASE("Pipeline build produces object file") {
  io::TempDir dir = io::TempDir::create_unique("pipeline_build_object_test_");
  const bool setup =
      dir.write_file("main.al", "fn main() {\n  print(\"hi\")\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }

  PipelineContext ctx;
  base::Result<path::Path, path::PathError> root =
      path::Path::from_native(dir.path());
  CHECK(root.is_ok());
  if (!root.is_ok()) {
    return;
  }

  const std::string obj_path = std::string(dir.path()) + "/main.o";
  auto res = pipeline::build_single_file(ctx, dir.join("main.al"), obj_path,
                                         false, "");
  CHECK(res.is_ok());
}

TEST_CASE("Pipeline build produces executable") {
  io::TempDir dir = io::TempDir::create_unique("pipeline_build_exe_test_");
  const bool setup =
      dir.write_file("main.al", "fn main() -> i32 {\n  ret 3\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }

  PipelineContext ctx;
  const std::string exe_path = std::string(dir.path()) + "/main_exe";
  auto res = pipeline::build_single_file(ctx, dir.join("main.al"), exe_path,
                                         false, "");
  CHECK(res.is_ok());
}

TEST_CASE("Pipeline build reports an unwritable object path") {
  io::TempDir dir = io::TempDir::create_unique("pipeline_build_bad_output_");
  const bool setup = dir.write_file("main.al", "fn main() {\n}\n") &&
                     dir.write_file("blocker", "not a directory\n");
  CHECK(setup);
  if (!setup) {
    return;
  }

  PipelineContext ctx;
  // A regular file blocks directory creation, so neither creating the
  // parent nor writing the object can succeed.
  const std::string bad_path = std::string(dir.path()) + "/blocker/main.o";
  auto res = pipeline::build_single_file(ctx, dir.join("main.al"), bad_path,
                                         false, "");
  CHECK(res.is_err());
  CHECK(ctx.bag.has_errors());
}
#endif

}  // namespace pipeline
