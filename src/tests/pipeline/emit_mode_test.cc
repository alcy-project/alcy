// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "pipeline/build.h"
#include "pipeline/pipeline_context.h"

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

TEST_CASE("Each emit mode names itself back") {
  CHECK(emit_mode_name(EmitMode::Executable) == "executable");
  CHECK(emit_mode_name(EmitMode::Object) == "object");
  CHECK(emit_mode_name(EmitMode::LlvmIr) == "llvm-ir");
}

TEST_CASE("A build writes what the mode asked for") {
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_mode_test_");
  const std::string source = dir.join("main.al");
  CHECK(dir.write_file("main.al", PROGRAM));

  // The extension is a default, not a switch: an object called `out.bin`
  // and a module called `out` are both what was asked for.
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
  CHECK(!io::is_file(dir.join("main.o")));

  {
    PipelineContext ctx;
    base::Result<void, diag::Reported> built = build_single_file(
        ctx, dir.join("main.al"), "", false, "", EmitMode::Object);
    CHECK(built.is_ok());
  }
  CHECK(io::is_file(dir.join("main.o")));
}

}  // namespace pipeline
