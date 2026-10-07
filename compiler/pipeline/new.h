// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/vcs.h"

namespace pipeline {

bool valid_package_name(std::string_view name);

// What a scaffold created. `name` is the package or suite name; `init`
// derives a package's from the directory rather than being given it, so
// the name is the result and not an argument the caller could already
// have echoed back.
struct ScaffoldResult {
  std::string name;
  // The suite the package joined, spelled `owner/name` or `name`;
  // empty when the package is standalone or the scaffold is a suite.
  std::string suite;
};

using NewResult = base::Result<ScaffoldResult, i32>;

// Creates the directory and writes alcy.toml, main.al, and (unless the
// package joins a suite) .gitignore. A path with slashes creates the
// directories it names, the package name is its last segment, and the
// package joins the nearest ancestor suite when one encloses it
// (ADR-0057).
NewResult create_new_package(PipelineContext& ctx,
                             std::string_view target_dir,
                             Vcs vcs);

// Creates alcy.toml and main.al inside an existing directory, deriving
// the package name from the directory itself; "." means the process's
// own directory. A package created inside a suite joins it, like
// create_new_package.
NewResult init_package(PipelineContext& ctx,
                       std::string_view target_dir,
                       Vcs vcs);

// Creates a suite manifest. `new` makes the directory, named after the
// suite unless target_dir names one; `init` writes into an existing
// directory. `spec` is `<name>` or `<owner>/<name>`, each segment
// validated like a package name.
NewResult create_new_suite(PipelineContext& ctx,
                           std::string_view target_dir,
                           std::string_view spec,
                           Vcs vcs);

NewResult init_suite(PipelineContext& ctx,
                     std::string_view target_dir,
                     std::string_view spec,
                     Vcs vcs);

}  // namespace pipeline
