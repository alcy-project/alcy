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
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/emit_mode.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/spawn.h"
#include "pipeline/std_select.h"
#include "pipeline/std_stage.h"
#include "source/source.h"

namespace pipeline {

#if !BUILD_FLAG(IS_OS_ASMJS)
TEST_CASE("Pipeline stages the standard library prelude") {
  PipelineContext ctx;
  base::Result<std::span<const analyzer::ModuleInput>, diag::Reported> prelude =
      std_prelude(ctx, pipeline::full_std_selection());
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
  // One entry module per package of the `alcy/std` suite, named by its
  // path within the suite; see docs/adr/0016.
  CHECK(inputs[0].name == "core/prelude.al");
  CHECK(inputs[0].id != source::UNKNOWN_FILE);
  for (const analyzer::ModuleInput& input : inputs) {
    CHECK(input.id != source::UNKNOWN_FILE);
  }
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
  auto res = pipeline::build_single_file(
      ctx, dir.join("main.al"), obj_path, false, pipeline::LinkOptions{},
      pipeline::EmitMode::Object, pipeline::full_std_selection());
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
  auto res = pipeline::build_single_file(
      ctx, dir.join("main.al"), exe_path, false, pipeline::LinkOptions{},
      pipeline::EmitMode::Executable, pipeline::full_std_selection());
  CHECK(res.is_ok());
}

TEST_CASE("Pipeline release build produces a working executable") {
  io::TempDir dir = io::TempDir::create_unique("pipeline_build_release_test_");
  const bool setup =
      dir.write_file("main.al", "fn main() -> i32 {\n  ret 3\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }

  PipelineContext ctx;
  const std::string exe_path =
      std::string(dir.path()) + "/main_exe" + std::string(exe_suffix());
  auto res = pipeline::build_single_file(
      ctx, dir.join("main.al"), exe_path, true, pipeline::LinkOptions{},
      pipeline::EmitMode::Executable, pipeline::full_std_selection());
  CHECK(res.is_ok());
  if (res.is_err()) {
    return;
  }
  // Release runs the O3 pipeline; the binary must still exit 3.
  base::Result<i32, SpawnError> ran = run_command({exe_path});
  CHECK(ran.is_ok());
  if (ran.is_ok()) {
    CHECK(std::move(ran).unwrap() == 3);
  }
}

// The emitter spills every local, so an unoptimized module has an alloca
// per one and the O3 pipeline's mem2reg promotes them. That makes the
// program's spills the difference between the flag reaching the IR and
// the flag reaching only the object file, which is how a release build
// once produced byte-identical IR either way. The runtime is defined in
// the module too, and its `alcy_alloc` keeps one alloca of its own -
// `posix_memalign` writes its result through a pointer - so the two are
// told apart by element type.
TEST_CASE("Release optimizes the textual IR, not only the object") {
  io::TempDir dir = io::TempDir::create_unique("pipeline_ir_optimize_");
  const std::string source =
      "fn add(a: i32, b: i32) -> i32 {\n  ret a + b\n}\n"
      "fn main() -> i32 {\n  _ := add(1i32, 2i32)\n  ret 0\n}\n";
  const bool setup = dir.write_file("main.al", source);
  CHECK(setup);
  if (!setup) {
    return;
  }

  io::TempDir outputs = io::TempDir::create_unique("pipeline_ir_out_");
  const std::string plain = outputs.join("plain.ll");
  const std::string released = outputs.join("released.ll");

  for (const auto& [optimize, target] :
       {std::pair{false, plain}, std::pair{true, released}}) {
    PipelineContext ctx;
    base::Result<std::string, diag::Reported> built = build_single_file(
        ctx, dir.join("main.al"), target, optimize, pipeline::LinkOptions{},
        pipeline::EmitMode::LlvmIr, pipeline::full_std_selection());
    CHECK(built.is_ok());
  }

  const std::string plain_ir = io::read_file(plain);
  const std::string released_ir = io::read_file(released);

  // Plain LLVM-IR should be like:
  // define i32 @"_A1f0:.3:add"(i32 %0, i32 %1) local_unnamed_addr #1 {
  //   %3 = add i32 %1, %0
  //   ret i32 %3
  // }
  // // or:
  // define i32 @"_A1f0:.3:add"(i32 %0, i32 %1) {
  //   %3 = alloca i32, align 4
  //   store i32 %0, ptr %3, align 4
  //   %4 = load i32, ptr %3, align 4
  //   %5 = alloca i32, align 4
  //   store i32 %4, ptr %5, align 4
  //   %6 = alloca i32, align 4
  //   store i32 %1, ptr %6, align 4
  //   %7 = load i32, ptr %6, align 4
  //   %8 = alloca i32, align 4
  //   store i32 %7, ptr %8, align 4
  //   %9 = load i32, ptr %5, align 4
  //   %10 = load i32, ptr %8, align 4
  //   %11 = add i32 %9, %10
  //   ret i32 %11
  // }
  //
  // define i32 @"_A1f0:.9:alcy_main"() {
  //   %1 = call i32 @"_A1f0:.3:add"(i32 1, i32 2)
  //   ret i32 0
  // }

  // Release LLVM-IR should be like:
  // define noundef i32 @"_A1f0:.9:alcy_main"() local_unnamed_addr #3 {
  //   ret i32 0
  // }

  CHECK(!plain_ir.empty());
  CHECK(!released_ir.empty());

  // Both have "alcy_main" function
  CHECK(plain_ir.find("alcy_main") != std::string::npos);
  CHECK(plain_ir.find("ret i32 0") != std::string::npos);
  CHECK(released_ir.find("alcy_main") != std::string::npos);
  CHECK(released_ir.find("ret i32 0") != std::string::npos);

  // Call of "add" at least exists on plain
  CHECK(plain_ir.find("add") != std::string::npos);
  CHECK(plain_ir.find("i32 1, i32 2") != std::string::npos);

  CHECK(plain_ir != released_ir);
}

TEST_CASE("A build creates the directory its output names") {
  // The linker creates no directories of its own, so `build -o out/app`
  // used to fail where the same -o for an object worked. The output's
  // parent is made once, before the mode is acted on.
  io::TempDir dir = io::TempDir::create_unique("pipeline_nested_output_");
  const std::string target = dir.join("a/b/c/app");
  const std::string out = target.substr(0, target.find_last_of('/')) + "/app";
  const bool setup =
      dir.write_file("main.al", "fn main() -> i32 {\n  ret 0\n}\n");
  CHECK(setup);
  if (!setup) {
    return;
  }
  PipelineContext ctx;
  base::Result<std::string, diag::Reported> built = build_single_file(
      ctx, dir.join("main.al"), out, false, pipeline::LinkOptions{},
      pipeline::EmitMode::Executable, pipeline::full_std_selection());
  CHECK(built.is_ok());
  CHECK(io::is_file(out));
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
  auto res = pipeline::build_single_file(
      ctx, dir.join("main.al"), bad_path, false, pipeline::LinkOptions{},
      pipeline::EmitMode::Object, pipeline::full_std_selection());
  CHECK(res.is_err());
  CHECK(ctx.bag.has_errors());
}
#endif

}  // namespace pipeline
