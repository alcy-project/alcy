// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/hash.h"
#include "codegen/target.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/io/io_util.h"
#include "fpag/io/temp_dir.h"
#include "i18n/language.h"
#include "ir/deserialize.h"
#include "ir/type.h"
#include "ir/write_input.h"
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

// Builds `source` in `dir` for `target` as an IR binary and answers the
// identity the artifact records, or nothing when a step failed.
std::optional<base::Digest128> identity_of(io::TempDir& dir,
                                           std::string_view source,
                                           const codegen::Target& target) {
  if (!dir.write_file("main.al", source)) {
    return std::nullopt;
  }
  PipelineContext ctx{i18n::Language::EnUs};
  ctx.target = target;
  const base::Result<std::string, diag::Reported> built = build_single_file(
      ctx, dir.join("main.al"), "", BuildProfile{}, LinkOptions{},
      EmitMode::IrBinary, full_std_selection());
  if (built.is_err()) {
    return std::nullopt;
  }
  const std::optional<std::string> bytes = io::read_file(dir.join("main.irb"));
  if (!bytes.has_value()) {
    return std::nullopt;
  }
  const std::span<const u8> written(reinterpret_cast<const u8*>(bytes->data()),
                                    bytes->size());
  const base::Result<ir::LoadedIr, ir::IrLoadError> loaded =
      ir::deserialize(written);
  if (loaded.is_err()) {
    return std::nullopt;
  }
  return loaded.value().identity;
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
    const ir::LoadedIr& package = loaded.value();
    CHECK(package.storage->functions().size() > 0);
    // The FILES section carries each source's content hash, so a cache
    // can tell two programs under one name apart. The staged standard
    // library is loaded too, so the program's own file is found by
    // name.
    const std::vector<std::string>& names = package.file_names;
    CHECK(names.size() == package.file_hashes.size());
    usize program = names.size();
    for (usize i = 0; i < names.size(); ++i) {
      if (names[i].ends_with("main.al")) {
        program = i;
      }
    }
    CHECK(program < names.size());
    if (program < names.size()) {
      const std::span<const u8> source(
          reinterpret_cast<const u8*>(PROGRAM.data()), PROGRAM.size());
      CHECK(package.file_hashes[program] == base::hash128(source));
    }
    // The identity is one digest over the tables the reader just
    // rebuilt: the writer computed it from the same facts, so a cache
    // compares one number and the table's own bytes prove it.
    CHECK(package.identity != base::Digest128{});
    std::vector<std::string_view> views;
    views.reserve(names.size());
    for (const std::string& name : names) {
      views.push_back(name);
    }
    CHECK(package.identity ==
          ir::identity_digest(
              ir::FileTable{.names = views, .hashes = package.file_hashes},
              package.width, package.compiler_version));
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

TEST_CASE("The identity follows the bytes a build read") {
  io::TempDir dir = io::TempDir::create_unique("alcy_ir_identity_test_");
  const codegen::Target host{"x86_64-unknown-linux-gnu", ir::PointerWidth::W64};
  const std::optional<base::Digest128> first = identity_of(dir, PROGRAM, host);
  const std::optional<base::Digest128> again = identity_of(dir, PROGRAM, host);
  const std::optional<base::Digest128> changed =
      identity_of(dir, std::string(PROGRAM) + "\n", host);
  CHECK(first.has_value());
  CHECK(again.has_value());
  CHECK(changed.has_value());
  // One digest per build: the same bytes hash the same, and one more
  // byte in the source is a different build.
  CHECK(first == again);
  CHECK(first != changed);
}

TEST_CASE("The identity does not follow the named machine") {
  // The IR is shared between machines of one width, and the identity is
  // over the IR's inputs, so a cache answers the second machine without
  // rebuilding.
  io::TempDir dir = io::TempDir::create_unique("alcy_ir_identity_machine_");
  const std::optional<base::Digest128> host = identity_of(
      dir, PROGRAM,
      codegen::Target{"x86_64-unknown-linux-gnu", ir::PointerWidth::W64});
  const std::optional<base::Digest128> cross = identity_of(
      dir, PROGRAM,
      codegen::Target{"aarch64-unknown-linux-gnu", ir::PointerWidth::W64});
  CHECK(host.has_value());
  CHECK(cross.has_value());
  CHECK(host == cross);
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
