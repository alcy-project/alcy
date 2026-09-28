// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "config/build_config.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

namespace {

// The extension each mode's output gets, and the first bytes that say what
// the file is. An object is an ELF relocatable; the IR is text starting
// with LLVM's module id line.
std::string read_file(const std::string& path) {
  const std::optional<std::string> text = io::read_file(path);
  return text.value_or(std::string{});
}

constexpr std::string_view PROGRAM =
    "fn main() -> i32 {\n"
    "  print(\"hi\")\n"
    "  ret 0\n"
    "}\n";

}  // namespace

TEST_CASE("Emit modes are named by their CLI spelling") {
  CHECK(parse_emit_mode("executable") == EmitMode::Executable);
  CHECK(parse_emit_mode("object") == EmitMode::Object);
  CHECK(parse_emit_mode("llvm-ir") == EmitMode::LlvmIr);
  // The aliases the parser offers, so a user is not forced to remember
  // which spelling the flag prefers.
  CHECK(parse_emit_mode("exe") == EmitMode::Executable);
  CHECK(parse_emit_mode("obj") == EmitMode::Object);
  CHECK(parse_emit_mode("ir") == EmitMode::LlvmIr);
}

TEST_CASE("An unknown emit mode is not a mode") {
  // The parser rejects these first, through the argument's choices; this
  // is the second line of defence for a caller that builds a config
  // directly.
  CHECK(!parse_emit_mode("").has_value());
  CHECK(!parse_emit_mode("bitcode").has_value());
  CHECK(!parse_emit_mode("LLVM-IR").has_value());
  CHECK(!parse_emit_mode(".o").has_value());
}

// An object needs a target machine, and the wasm build has none linked in,
// so the object half of these cases is compiled out there. The IR half is
// not: printing a module needs no backend, which is the property that makes
// it the one output available before a target is chosen.
#if !BUILD_FLAG(IS_OS_ASMJS)
TEST_CASE("A build writes an object where it was asked for one") {
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_object_test_");
  const std::string source = dir.join("main.al");
  CHECK(dir.write_file("main.al", PROGRAM));

  // The extension is a default, not a switch: an object called `out.bin` is
  // still what was asked for.
  const std::string object_path = dir.join("out.bin");
  {
    PipelineContext ctx;
    base::Result<void, diag::Reported> built = build_single_file(
        ctx, source, object_path, false, "", EmitMode::Object);
    CHECK(built.is_ok());
  }
  const std::string object = read_file(object_path);
  CHECK(object.size() > 4);
  if (object.size() > 4) {
    // ELF magic: a relocatable, not a script we happened to write.
    CHECK(object.compare(0, 4,
                         "\x7f"
                         "ELF") == 0);
  }
}
#endif  // !BUILD_FLAG(IS_OS_ASMJS

TEST_CASE("A build writes the module as textual IR") {
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_ir_test_");
  const std::string source = dir.join("main.al");
  CHECK(dir.write_file("main.al", PROGRAM));

  const std::string ir_path = dir.join("out");
  {
    PipelineContext ctx;
    base::Result<void, diag::Reported> built =
        build_single_file(ctx, source, ir_path, false, "", EmitMode::LlvmIr);
    CHECK(built.is_ok());
  }
  const std::string ir = read_file(ir_path);
  CHECK(!ir.empty());
  // The one property that makes textual IR worth having: it is readable,
  // and it names what the program contains.
  CHECK(ir.find("define") != std::string::npos);
  CHECK(ir.find("hi") != std::string::npos);
  CHECK(ir.find("alcy_module") != std::string::npos);
}

TEST_CASE("The default extension follows the mode") {
  // No output named: the source's extension is replaced by the mode's.
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_suffix_test_");
  CHECK(dir.write_file("main.al", PROGRAM));

  {
    PipelineContext ctx;
    base::Result<void, diag::Reported> built = build_single_file(
        ctx, dir.join("main.al"), "", false, "", EmitMode::LlvmIr);
    CHECK(built.is_ok());
  }
  CHECK(io::is_file(dir.join("main.ll")));
#if !BUILD_FLAG(IS_OS_ASMJS)
  CHECK(!io::is_file(dir.join("main.o")));

  {
    PipelineContext ctx;
    base::Result<void, diag::Reported> built = build_single_file(
        ctx, dir.join("main.al"), "", false, "", EmitMode::Object);
    CHECK(built.is_ok());
  }
  CHECK(io::is_file(dir.join("main.o")));
#endif  // !BUILD_FLAG(IS_OS_ASMJS
}

TEST_CASE("A package build honours the mode too") {
  // A package puts an executable in out/ and an object beside the
  // manifest, so the two modes do not land in the same place.
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_package_test_");
  const bool setup =
      dir.write_file(
          "proj/alcy.toml",
          "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[[bin]]\n"
          "name = \"app\"\npath = \"main.al\"\n") &&
      dir.write_file("proj/main.al", PROGRAM);
  CHECK(setup);
  if (!setup) {
    return;
  }

  struct Case {
    EmitMode mode;
    const char* relative;
  };
  const Case cases[] = {
      {EmitMode::LlvmIr, "proj/app.ll"},
#if !BUILD_FLAG(IS_OS_ASMJS)
      {EmitMode::Object, "proj/app.o"},
#endif  // !BUILD_FLAG(IS_OS_ASMJS
  };
  for (const Case& one : cases) {
    INFO("mode " << static_cast<u32>(one.mode));
    PipelineContext ctx;
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
    base::Result<void, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", false, "", one.mode);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join(one.relative)));
#if !BUILD_FLAG(IS_OS_ASMJS)
    // An object never ends up in the executable's directory.
    CHECK(!io::is_file(dir.join("proj/out/app.o")));
#endif  // !BUILD_FLAG(IS_OS_ASMJS
  }
}

}  // namespace pipeline
