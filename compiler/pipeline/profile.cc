// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/profile.h"

#include <optional>
#include <string_view>
#include <utility>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "i18n/messages.h"
#include "path/path.h"
#include "pipeline/diag_code.h"
#include "pipeline/pipeline_context.h"
#include "pkg/config.h"
#include "pkg/profile.h"
#include "source/source.h"

namespace pipeline {

namespace {

// What `release` sets before any table or flag speaks. Every other
// profile, built-in `debug` and the file's own alike, starts from
// nothing: a custom profile never inherits the release profile's
// values, and says `optimize = true` itself.
pkg::ProfileSettings builtin_base(std::string_view name) {
  if (name == "release") {
    return pkg::ProfileSettings{.optimize = true, .overflow_checks = false};
  }
  return pkg::ProfileSettings{};
}

bool is_builtin(std::string_view name) {
  return name == "debug" || name == "release";
}

}  // namespace

base::Result<pkg::Profiles, diag::Reported> load_profiles(
    PipelineContext& ctx,
    const path::Path& root) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "profile", "frontend");
  // The profile file is small, so the probe, load, and parse are one
  // request, with the load the only file access of the three.
  const path::Path path =
      root.join(pkg::CONFIG_DIR_NAME).join(pkg::PROFILE_FILE_NAME);
  base::Result<source::FileId, source::SourceError> file =
      ctx.sources.load(path.as_view());
  if (file.is_err()) {
    return base::make_ok(pkg::Profiles{});
  }
  const source::FileId loaded = std::move(file).unwrap();
  const std::optional<std::string_view> bytes = ctx.sources.bytes(loaded);
  if (!bytes.has_value()) {
    const u32 index = ctx.bag.emit<i18n::Key::PkgProfileNotLoaded>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::IoError,
        path.as_view());
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return pkg::parse_profile(*bytes, path.as_view(), loaded, ctx.bag, ctx.arena);
}

base::Result<BuildProfile, diag::Reported> resolve_profile(
    const ProfileRequest& request,
    const pkg::Profiles& profiles,
    diag::DiagBag& bag) {
  const std::string_view name =
      request.name.empty() ? DEFAULT_PROFILE_NAME : request.name;
  const pkg::ProfileTable* table = nullptr;
  for (const pkg::ProfileTable& candidate : profiles.tables) {
    if (candidate.name == name) {
      table = &candidate;
      break;
    }
  }
  if (table == nullptr && !is_builtin(name)) {
    const u32 index = bag.emit<i18n::Key::PipelineUnknownProfile>(
        diag::Severity::Error, diag::Stage::Pipeline, DiagCode::UnknownProfile,
        name);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  pkg::ProfileSettings settings = builtin_base(name);
  if (table != nullptr) {
    if (table->settings.optimize.has_value()) {
      settings.optimize = table->settings.optimize;
    }
    if (table->settings.on_panic.has_value()) {
      settings.on_panic = table->settings.on_panic;
    }
    if (table->settings.overflow_checks.has_value()) {
      settings.overflow_checks = table->settings.overflow_checks;
    }
  }
  if (request.on_panic.has_value()) {
    settings.on_panic = request.on_panic;
  }
  if (request.optimize.has_value()) {
    settings.optimize = request.optimize;
  }
  if (request.overflow_checks.has_value()) {
    settings.overflow_checks = request.overflow_checks;
  }
  return base::make_ok(
      BuildProfile{.optimize = settings.optimize.value_or(false),
                   .on_panic = settings.on_panic.value_or(pkg::OnPanic::Abort),
                   .overflow_checks = settings.overflow_checks.value_or(true)});
}

}  // namespace pipeline
