// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

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

// Links one object plus the staged runtime into an executable.
// Empty linker selects the default toolchain driver.
base::Result<void, diag::Reported> link_executable(
    PipelineContext& ctx,
    std::string_view linker,
    const std::string& object_path,
    const std::string& runtime_path,
    const std::string& exe_path);

base::Result<void, diag::Reported> build_single_file(PipelineContext& ctx,
                                                     std::string_view target,
                                                     std::string_view output,
                                                     bool optimize,
                                                     std::string_view linker);

base::Result<void, diag::Reported> build_package(PipelineContext& ctx,
                                                 const path::Path& root,
                                                 source::FileId manifest_file,
                                                 std::string_view manifest_name,
                                                 std::string_view output,
                                                 bool optimize,
                                                 std::string_view linker);

}  // namespace pipeline
