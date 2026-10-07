// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "ir/deserialize.h"
#include "pipeline/build.h"
#include "pipeline/emit_mode.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"

namespace pipeline {

namespace {

constexpr std::string_view PROGRAM =
    "fn main() -> i32 {\n"
    "  print(\"hi\")\n"
    "  ret 0\n"
    "}\n";

}  // namespace

TEST_CASE("The ir mode writes the lowered package beside the source") {
  io::TempDir dir = io::TempDir::create_unique("alcy_ir_emit_test_");
  CHECK(dir.write_file("main.al", PROGRAM));
  PipelineContext ctx{i18n::Language::EnUs};
  const base::Result<std::string, diag::Reported> built =
      build_single_file(ctx, dir.join("main.al"), "", false, LinkOptions{},
                        EmitMode::Ir, full_std_selection());
  CHECK(built.is_ok());
  const std::optional<std::string> text = io::read_file(dir.join("main.ir"));
  CHECK(text.has_value());
  if (!text.has_value()) {
    return;
  }
  // The width follows the run's target, so the wasm test host says 32.
  CHECK(text->starts_with("// alcy ir, format 1, pointer width "));
  CHECK(text->find("// prelude:") != std::string::npos);
  CHECK(text->find("fn main(") != std::string::npos);
}

TEST_CASE("The ir-bc mode writes the binary form beside the source") {
  io::TempDir dir = io::TempDir::create_unique("alcy_ir_bc_emit_test_");
  CHECK(dir.write_file("main.al", PROGRAM));
  PipelineContext ctx{i18n::Language::EnUs};
  const base::Result<std::string, diag::Reported> built =
      build_single_file(ctx, dir.join("main.al"), "", false, LinkOptions{},
                        EmitMode::IrBinary, full_std_selection());
  CHECK(built.is_ok());
  const std::optional<std::string> bytes = io::read_file(dir.join("main.irb"));
  CHECK(bytes.has_value());
  if (!bytes.has_value() || bytes->size() < 4) {
    return;
  }
  CHECK(bytes->starts_with("ALIR"));
  const std::span<const u8> written(reinterpret_cast<const u8*>(bytes->data()),
                                    bytes->size());
  const base::Result<ir::LoadedIr, ir::IrLoadError> loaded =
      ir::deserialize(written);
  CHECK(loaded.is_ok());
  if (loaded.is_ok()) {
    CHECK((*loaded.value().storage).functions().size() > 0);
  }
}

TEST_CASE("The ir mode refuses a release build") {
  io::TempDir dir = io::TempDir::create_unique("alcy_ir_release_test_");
  CHECK(dir.write_file("main.al", PROGRAM));
  PipelineContext ctx{i18n::Language::EnUs};
  const base::Result<std::string, diag::Reported> built =
      build_single_file(ctx, dir.join("main.al"), "", true, LinkOptions{},
                        EmitMode::Ir, full_std_selection());
  CHECK(built.is_err());

  PipelineContext binary_ctx{i18n::Language::EnUs};
  const base::Result<std::string, diag::Reported> binary = build_single_file(
      binary_ctx, dir.join("main.al"), "", true, LinkOptions{},
      EmitMode::IrBinary, full_std_selection());
  CHECK(binary.is_err());
}

}  // namespace pipeline
