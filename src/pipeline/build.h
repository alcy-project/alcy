// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/io/temp_dir.h"
#include "lowering/lowering.h"
#include "path/path.h"
#include "pipeline/build_plan.h"
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
    analyzer::ModuleTree tree,
    BuildPlan plan,
    bool needs_entry,
    std::string_view entry_name);

// Links the objects into an executable, in the order given. An empty driver
// in `link` selects the default toolchain driver, and its arguments follow
// the objects on the command line.
base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    LinkOptions link,
    std::span<const std::string> object_paths,
    const std::string& exe_path);

// How many objects a package's code generation is split into. A function of
// the program and not of the machine: a small package stays whole, and a
// large one gains nothing from parts finer than the work a core can take.
[[nodiscard]] u32 codegen_parts(const lowering::LoweredPackage& package);

// Emits the package's code generation as objects under `scratch`, one per
// part, and answers their paths in order. The part count is the caller's,
// and it must be a function of the program: two runs of one package write
// the same objects whatever `-j` said.
base::Result<std::vector<std::string>, diag::Reported> emit_package_objects(
    PipelineContext& ctx,
    lowering::LoweredPackage& package,
    BuildPlan plan,
    const io::TempDir& scratch,
    bool is_lib,
    u32 parts);

base::Result<std::string, diag::Reported> build_single_file(
    PipelineContext& ctx,
    std::string_view input,
    std::string_view output,
    BuildPlan plan,
    EmitMode mode,
    const StdSelection& selection);

// The input-independent tail of a single-file build: everything from
// a loaded root onwards, which does not care whether the bytes came
// from a file or a pipe. `output` names the artifact unconditionally.
base::Result<std::string, diag::Reported> build_single_root(
    PipelineContext& ctx,
    source::FileId root,
    std::string_view output,
    BuildPlan plan,
    EmitMode mode,
    const StdSelection& selection);

base::Result<std::string, diag::Reported> build_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    std::string_view output,
    BuildPlan plan,
    EmitMode mode);

// Builds every member of the suite at `root`, each through its own
// manifest and into its own directory under the suite's `out/`
// (ADR-0057). Success names that directory; `-o` is refused because it
// cannot name more than one member's artifact.
base::Result<std::string, diag::Reported> build_suite(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    std::string_view output,
    BuildPlan plan,
    EmitMode mode);

}  // namespace pipeline
