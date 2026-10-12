// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string_view>

#include "diag/bag.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/profile.h"

namespace pipeline {

// The profile a command starts from when nothing selects another.
constexpr std::string_view DEFAULT_PROFILE_NAME = "debug";

// The resolved settings one build runs with. Backends see these values,
// never a profile name.
struct BuildProfile {
  bool optimize = false;
  pkg::OnPanic on_panic = pkg::OnPanic::Abort;
  // Whether the source arithmetic's checks are materialized; `debug` and
  // a profile the file defines say on, `release` says off (ADR-0068).
  bool overflow_checks = true;
};

// What the command line selected: a profile name, and the keys an
// explicit flag overrides. An empty name selects the default; an unset
// override leaves the profile's value standing.
struct ProfileRequest {
  std::string_view name = DEFAULT_PROFILE_NAME;
  std::optional<pkg::OnPanic> on_panic = std::nullopt;
  std::optional<bool> optimize = std::nullopt;
  std::optional<bool> overflow_checks = std::nullopt;
};

// Loads `.alcy/profile.toml` beside `root`. An absent file is an empty
// set of tables, which is also what a command that reads no file
// passes.
base::Result<pkg::Profiles, diag::Reported> load_profiles(
    PipelineContext& ctx,
    const path::Path& root);

// Resolves `request` against the goal package's tables: the built-in
// profile's own values, the named table's keys over them, then the
// request's overrides. A name neither built in nor declared is a
// diagnostic.
base::Result<BuildProfile, diag::Reported> resolve_profile(
    const ProfileRequest& request,
    const pkg::Profiles& profiles,
    diag::DiagBag& bag);

}  // namespace pipeline
