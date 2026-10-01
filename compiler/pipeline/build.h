// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lowering/lowering.h"
#include "path/path.h"
#include "pipeline/emit_mode.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace pipeline {

// Shared frontend: type checking, lowering, and borrow checking over a
// resolved tree. Used by build and run so both lower identical IR.
base::Result<lowering::LoweredPackage, diag::Reported> compile_tree(
    PipelineContext& ctx,
    analyzer::ModuleTree tree);

// Emits one relocatable object for a lowered package. The module is
// built, runtime included, and, when `optimize` is set, put through the
// O3 pipeline here rather than inside the emission, so that every
// backend sees the same module. Write failures land in the bag; the
// output's parent directory is the caller's to have made.
base::Result<void, diag::Reported> emit_package_object(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    bool optimize,
    const std::string& output_path);

// Writes the module as LLVM's textual IR, optimized or not on the same
// terms as emit_package_object.
base::Result<void, diag::Reported> emit_package_ir(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    bool optimize,
    const std::string& output_path);

// Writes the module as LLVM's bitcode, on the same terms as
// emit_package_ir.
base::Result<void, diag::Reported> emit_package_bitcode(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    bool optimize,
    const std::string& output_path);

// Links one object into an executable. An empty driver in `link`
// selects the default toolchain driver, and its arguments follow the
// object on the command line.
base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    LinkOptions link,
    const std::string& object_path,
    const std::string& exe_path);

// Emits lowered IR according to the mode: one object, textual IR,
// bitcode, or an object linked into an executable. Both single-file and
// package builds end here. Success carries the path written, which the
// caller cannot work out on its own when the caller left the path
// empty.
base::Result<std::string, diag::Reported> emit_output(
    PipelineContext& ctx,
    lowering::LoweredPackage& lowered,
    bool optimize,
    LinkOptions link,
    EmitMode mode,
    const std::string& output_path);

base::Result<std::string, diag::Reported> build_single_file(
    PipelineContext& ctx,
    std::string_view target,
    std::string_view output,
    bool optimize,
    LinkOptions link,
    EmitMode mode,
    const StdSelection& selection);

// The target-independent tail of a single-file build: everything from
// a loaded root onwards, which does not care whether the bytes came
// from a file or a pipe. `output` names the artifact unconditionally.
base::Result<std::string, diag::Reported> build_single_root(
    PipelineContext& ctx,
    source::FileId root,
    std::string_view output,
    bool optimize,
    LinkOptions link,
    EmitMode mode,
    const StdSelection& selection);

base::Result<std::string, diag::Reported> build_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    std::string_view output,
    bool optimize,
    LinkOptions link,
    EmitMode mode);

}  // namespace pipeline
