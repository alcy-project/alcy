// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/emit_mode.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "config/build_config.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "path/path.h"
#include "pipeline/build.h"
#include "pipeline/diag_code.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"
#include "tests/util/test_util.h"

namespace pipeline {

namespace {

// The extension each mode's output gets, and the first bytes that say what
// the file is. An object is a relocatable in the platform's format; the IR
// is text starting with LLVM's module id line.
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
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<std::string, diag::Reported> built =
        build_single_file(ctx, source, object_path, false, LinkOptions{},
                          EmitMode::Object, pipeline::full_std_selection());
    CHECK(built.is_ok());
  }
  const std::string object = read_file(object_path);
  // Object magic: a relocatable, not a script we happened to write.
  CHECK(tests::is_object_bytes(object));
}
#endif  // !BUILD_FLAG(IS_OS_ASMJS

TEST_CASE("A build writes the module as textual IR") {
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_ir_test_");
  const std::string source = dir.join("main.al");
  CHECK(dir.write_file("main.al", PROGRAM));

  const std::string ir_path = dir.join(path::DEFAULT_OUT_DIR);
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<std::string, diag::Reported> built =
        build_single_file(ctx, source, ir_path, false, LinkOptions{},
                          EmitMode::LlvmIr, pipeline::full_std_selection());
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

TEST_CASE("A build writes the module as bitcode") {
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_bc_test_");
  const std::string source = dir.join("main.al");
  CHECK(dir.write_file("main.al", PROGRAM));

  // A named output keeps its own extension, so the bytes are the only
  // thing left that says what was written.
  const std::string bitcode_path = dir.join("out.bin");
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<std::string, diag::Reported> built = build_single_file(
        ctx, source, bitcode_path, false, LinkOptions{}, EmitMode::LlvmBitcode,
        pipeline::full_std_selection());
    CHECK(built.is_ok());
  }
  CHECK(tests::is_bitcode_bytes(read_file(bitcode_path)));
}

TEST_CASE("The default extension follows the mode") {
  // No output named: the source's extension is replaced by the mode's.
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_suffix_test_");
  CHECK(dir.write_file("main.al", PROGRAM));

  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<std::string, diag::Reported> built =
        build_single_file(ctx, dir.join("main.al"), "", false, LinkOptions{},
                          EmitMode::LlvmIr, pipeline::full_std_selection());
    CHECK(built.is_ok());
  }
  CHECK(io::is_file(dir.join("main.ll")));
  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<std::string, diag::Reported> built = build_single_file(
        ctx, dir.join("main.al"), "", false, LinkOptions{},
        EmitMode::LlvmBitcode, pipeline::full_std_selection());
    CHECK(built.is_ok());
  }
  CHECK(io::is_file(dir.join("main.bc")));
#if !BUILD_FLAG(IS_OS_ASMJS)
  CHECK(!io::is_file(dir.join("main.o")));

  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<std::string, diag::Reported> built =
        build_single_file(ctx, dir.join("main.al"), "", false, LinkOptions{},
                          EmitMode::Object, pipeline::full_std_selection());
    CHECK(built.is_ok());
  }
  CHECK(io::is_file(dir.join("main.o")));
#endif  // !BUILD_FLAG(IS_OS_ASMJS
}

TEST_CASE("A source without an extension still gets an output name") {
  // `compile noext` used to abort: the derivation assumed a dot and there
  // was none to replace. The suffix is appended instead, and a mode with
  // no suffix at all - the executable one on POSIX - reports that it
  // cannot name the artifact rather than writing it over the source at
  // the same path.
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_noext_test_");
  CHECK(dir.write_file("noext", PROGRAM));

  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<std::string, diag::Reported> built =
        build_single_file(ctx, dir.join("noext"), "", false, LinkOptions{},
                          EmitMode::Object, pipeline::full_std_selection());
    CHECK(built.is_ok());
  }
  CHECK(io::is_file(dir.join("noext.o")));

  {
    PipelineContext ctx{i18n::Language::EnUs};
    base::Result<std::string, diag::Reported> refused =
        build_single_file(ctx, dir.join("noext"), "", false, LinkOptions{},
                          EmitMode::Executable, pipeline::full_std_selection());
    CHECK(refused.is_err());
    const diag::Diagnostic* const only = ctx.bag.at(0);
    CHECK(only != nullptr);
    if (only != nullptr) {
      CHECK(only->code == diag::Code{diag::Stage::Pipeline,
                                     static_cast<u8>(DiagCode::NoOutputName)});
    }
  }
  // The source is untouched: the refused case named no output, and the
  // one that only appends never has a reason to write to it.
  CHECK(read_file(dir.join("noext")) == PROGRAM);
}

TEST_CASE("A dot in a directory name is not an extension") {
  // `sub.dir/main.al` must lose `.al` and keep `.dir`, whichever dot
  // comes last in the whole string.
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_dotdir_test_");
  CHECK(dir.write_file("sub.dir/main.al", PROGRAM));

  PipelineContext ctx{i18n::Language::EnUs};
  base::Result<std::string, diag::Reported> built = build_single_file(
      ctx, dir.join("sub.dir/main.al"), "", false, LinkOptions{},
      EmitMode::Object, pipeline::full_std_selection());
  CHECK(built.is_ok());
  CHECK(io::is_file(dir.join("sub.dir/main.o")));
}

TEST_CASE("A package build honours the mode too") {
  // A package puts an executable in out/ and an object beside the
  // manifest, so the two modes do not land in the same place.
  io::TempDir dir = io::TempDir::create_unique("alcy_emit_package_test_");
  const bool setup =
      dir.write_file("proj/alcy.toml",
                     "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n"
                     "[dependencies]\n\"alcy/std/*\" = {}\n\n[[bin]]\n"
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
      {EmitMode::LlvmIr, "proj/out/app.ll"},
      {EmitMode::LlvmBitcode, "proj/out/app.bc"},
#if !BUILD_FLAG(IS_OS_ASMJS)
      {EmitMode::Object, "proj/out/app.o"},
#endif  // !BUILD_FLAG(IS_OS_ASMJS
  };
  for (const Case& one : cases) {
    INFO("mode " << static_cast<u32>(one.mode));
    PipelineContext ctx{i18n::Language::EnUs};
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
    base::Result<std::string, diag::Reported> built =
        build_package(ctx, root_path, std::move(manifest).unwrap(), "alcy.toml",
                      "", false, LinkOptions{}, one.mode);
    CHECK(built.is_ok());
    CHECK(io::is_file(dir.join(one.relative)));
    // Every mode lands in the directory the scaffold's own `.gitignore`
    // names, so a build never drops an artifact where git can see it.
    CHECK(!io::is_file(dir.join("proj/app.ll")));
    CHECK(!io::is_file(dir.join("proj/app.bc")));
#if !BUILD_FLAG(IS_OS_ASMJS)
    CHECK(!io::is_file(dir.join("proj/app.o")));
#endif  // !BUILD_FLAG(IS_OS_ASMJS
  }
}

}  // namespace pipeline
