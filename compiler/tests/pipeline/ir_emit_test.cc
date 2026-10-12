// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "ir/deserialize.h"
#include "ir/type.h"
#include "pipeline/build.h"
#include "pipeline/emit_mode.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/profile.h"
#include "pipeline/std_select.h"

namespace pipeline {

namespace {

constexpr std::string_view PROGRAM =
    "fn main() -> i32 {\n"
    "  print(\"hi\")\n"
    "  ret 0\n"
    "}\n";

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  usize start = 0;
  while (start <= text.size()) {
    const usize end = text.find('\n', start);
    if (end == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

}  // namespace

TEST_CASE("The ir mode writes the lowered package beside the source") {
  io::TempDir dir = io::TempDir::create_unique("alcy_ir_emit_test_");
  CHECK(dir.write_file("main.al", PROGRAM));
  PipelineContext ctx{i18n::Language::EnUs};
  const base::Result<std::string, diag::Reported> built =
      build_single_file(ctx, dir.join("main.al"), "", BuildProfile{},
                        LinkOptions{}, EmitMode::Ir, full_std_selection());
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
  const base::Result<std::string, diag::Reported> built = build_single_file(
      ctx, dir.join("main.al"), "", BuildProfile{}, LinkOptions{},
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

TEST_CASE("Two 64-bit targets lower one source to the same IR") {
  // One directory for both builds: the spans in the text form name the
  // source path, so two directories would differ for a reason that is
  // not the machine.
  io::TempDir dir = io::TempDir::create_unique("alcy_ir_target_");
  CHECK(dir.write_file("main.al", PROGRAM));

  PipelineContext host_ctx{i18n::Language::EnUs};
  host_ctx.target =
      codegen::Target{"x86_64-unknown-linux-gnu", ir::PointerWidth::W64};
  const base::Result<std::string, diag::Reported> host_built =
      build_single_file(host_ctx, dir.join("main.al"), "", BuildProfile{},
                        LinkOptions{}, EmitMode::Ir, full_std_selection());
  CHECK(host_built.is_ok());
  const std::optional<std::string> host_text =
      io::read_file(dir.join("main.ir"));

  PipelineContext cross_ctx{i18n::Language::EnUs};
  cross_ctx.target =
      codegen::Target{"aarch64-unknown-linux-gnu", ir::PointerWidth::W64};
  const base::Result<std::string, diag::Reported> cross_built =
      build_single_file(cross_ctx, dir.join("main.al"), "", BuildProfile{},
                        LinkOptions{}, EmitMode::Ir, full_std_selection());
  CHECK(cross_built.is_ok());
  const std::optional<std::string> cross_text =
      io::read_file(dir.join("main.ir"));

  CHECK(host_text.has_value());
  CHECK(cross_text.has_value());
  if (host_text.has_value() && cross_text.has_value()) {
    // The IR is the artifact a cache would share between profiles and
    // machines of one width, so nothing about the named machine may
    // reach it. Compare line by line: a first difference names itself
    // instead of printing two full dumps.
    const std::vector<std::string> host_lines = split_lines(*host_text);
    const std::vector<std::string> cross_lines = split_lines(*cross_text);
    CHECK(host_lines.size() == cross_lines.size());
    usize first = std::min(host_lines.size(), cross_lines.size());
    for (usize i = 0; i < first; ++i) {
      if (host_lines[i] != cross_lines[i]) {
        first = i;
        break;
      }
    }
    CHECK_MESSAGE(first == host_lines.size(), "first difference at line ",
                  first, ": '",
                  first < host_lines.size() ? host_lines[first] : "", "' and '",
                  first < cross_lines.size() ? cross_lines[first] : "", "'");
  }
}

TEST_CASE("The ir mode answers a release build the same artifact") {
  io::TempDir dir = io::TempDir::create_unique("alcy_ir_release_test_");
  CHECK(dir.write_file("main.al", PROGRAM));
  PipelineContext ctx{i18n::Language::EnUs};
  const base::Result<std::string, diag::Reported> built = build_single_file(
      ctx, dir.join("main.al"), "", BuildProfile{.optimize = true},
      LinkOptions{}, EmitMode::Ir, full_std_selection());
  // The unoptimized IR form is the cacheable artifact and does not
  // follow the profile, so a release build is answered, not refused
  // (ADR-0067, amended).
  CHECK(built.is_ok());

  PipelineContext binary_ctx{i18n::Language::EnUs};
  const base::Result<std::string, diag::Reported> binary = build_single_file(
      binary_ctx, dir.join("main.al"), "", BuildProfile{.optimize = true},
      LinkOptions{}, EmitMode::IrBinary, full_std_selection());
  CHECK(binary.is_ok());
}

}  // namespace pipeline
