// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>

#include "config/build_config.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

#if !BUILD_FLAG(IS_OS_ASMJS)
TEST_CASE("Pipeline build produces object file") {
  io::TempDir dir("pipeline_build_object_test");
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
  pipeline::report(ctx.bag, ctx.sources);
}

TEST_CASE("Pipeline build produces executable") {
  io::TempDir dir("pipeline_build_exe_test");
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
  if (res.is_err()) {
    pipeline::report(ctx.bag, ctx.sources);
  }
}

TEST_CASE("Pipeline build creates nonexistent directory") {
  io::TempDir dir("pipeline_build_bad_output");
  const bool setup = dir.write_file("main.al", "fn main() {\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }

  PipelineContext ctx;
  const std::string bad_path = std::string(dir.path()) + "/no-such-dir/main.o";
  auto res = pipeline::build_single_file(ctx, dir.join("main.al"), bad_path,
                                         false, "");
  CHECK(res.is_ok());
  CHECK(!ctx.bag.has_errors());
}
#endif

}  // namespace pipeline
