// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lower/lower.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

// What a build writes. The mode is chosen explicitly rather than inferred
// from the output's extension, because an extension says what a file is
// called and not what was asked for: `main.o` as a *name* is just as valid
// a request for an executable as `main` is a request for an object.
enum class EmitMode : u8 {
  // Compile, link against the runtime, and write an executable.
  Executable,
  // Write one relocatable object and stop.
  Object,
  // Write the module as LLVM's textual IR and stop. Needs no target, so it
  // is the one mode that works before a backend is chosen.
  LlvmIr,
};

// The mode named by `text`, or std::nullopt when it names no mode. The
// spelling is the CLI's: what follows `emit=`.
std::optional<EmitMode> parse_emit_mode(std::string_view text);

// Shared frontend: type checking, lowering, and borrow checking over a
// resolved tree. Used by build and run so both lower identical IR.
base::Result<lower::LoweredPackage, diag::Reported> compile_tree(
    PipelineContext& ctx,
    analyzer::ModuleTree tree);

// Emits one relocatable object for lowered IR. Write or emission
// failures land in the bag.
base::Result<void, diag::Reported> emit_package_object(
    PipelineContext& ctx,
    lower::LoweredPackage& package,
    bool optimize,
    const std::string& output_path);

// Writes the module as LLVM's textual IR.
base::Result<void, diag::Reported> emit_package_ir(
    PipelineContext& ctx,
    lower::LoweredPackage& package,
    const std::string& output_path);

// Links one object plus the staged runtime into an executable.
// Empty linker selects the default toolchain driver.
base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    std::string_view linker,
    const std::string& object_path,
    const std::string& runtime_path,
    const std::string& exe_path);

// Emits lowered IR according to the mode: one object, textual IR, or
// an object staged with the runtime and linked into an executable.
// Both single-file and package builds end here.
base::Result<void, diag::Reported> emit_output(PipelineContext& ctx,
                                               lower::LoweredPackage& lowered,
                                               bool optimize,
                                               std::string_view linker,
                                               EmitMode mode,
                                               const std::string& output_path);

base::Result<void, diag::Reported> build_single_file(PipelineContext& ctx,
                                                     std::string_view target,
                                                     std::string_view output,
                                                     bool optimize,
                                                     std::string_view linker,
                                                     EmitMode mode);

base::Result<void, diag::Reported> build_package(PipelineContext& ctx,
                                                 const path::Path& root,
                                                 source::FileId manifest_file,
                                                 std::string_view manifest_name,
                                                 std::string_view output,
                                                 bool optimize,
                                                 std::string_view linker,
                                                 EmitMode mode);

}  // namespace pipeline
