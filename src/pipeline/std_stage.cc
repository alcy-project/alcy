// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/std_stage.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "fpag/base/numeric.h"
#include "pipeline/embedded_std.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace pipeline {

namespace {

bool package_is_selected(const std::vector<std::string_view>& members,
                         std::string_view path) {
  const usize slash = path.find('/');
  const std::string_view package =
      slash == std::string_view::npos ? path : path.substr(0, slash);
  for (std::string_view member : members) {
    if (member == package) {
      return true;
    }
  }
  return false;
}

// A source with this name is its package's facade.
constexpr std::string_view FACADE_SUFFIX = "/prelude.al";

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string_view as_view(const u8* data, u64 len) {
  return std::string_view(reinterpret_cast<const char*>(data), len);
}

}  // namespace

std::span<const analyzer::ModuleInput> std_prelude(
    PipelineContext& ctx,
    const StdSelection& selection) {
  if (ctx.std_staged && ctx.std_selected == selection.members) {
    return std::span<const analyzer::ModuleInput>(ctx.std_inputs);
  }
  ctx.std_inputs.clear();
  ctx.std_selected = selection.members;
  // One facade per selected member plus the modules beside it, named by
  // the path within the suite. Anything unselected is absent, not merely
  // out of scope. The bytes come from the embedded table and are copied
  // into the source manager under that name, so nothing touches the
  // filesystem and two compilations never read each other's prelude.
  for (usize i = 0; i < STAGED_SOURCE_COUNT; ++i) {
    const StagedSource& source = STAGED_SOURCES[i];
    if (!package_is_selected(selection.members,
                             std::string_view(source.path))) {
      continue;
    }
    const source::FileId id = ctx.sources.add_virtual(
        source.path,
        as_view(source.data, static_cast<decltype(source.len)>(source.len)));
    // A source named `prelude.al` is its package's facade: its public
    // surface is in scope without a `use`. The name is read from the
    // embedded path, so a `prelude` module in a user package stays
    // ordinary.
    const bool facade = ends_with(source.path, FACADE_SUFFIX);
    ctx.std_inputs.push_back({std::string_view(source.path), id, facade});
  }
  ctx.std_staged = true;
  return std::span<const analyzer::ModuleInput>(ctx.std_inputs);
}

std::optional<StagedMember> staged_member(std::string_view path,
                                          const StdSelection& selection) {
  for (usize i = 0; i < STAGED_SOURCE_COUNT; ++i) {
    const std::string_view staged(STAGED_SOURCES[i].path);
    if (!package_is_selected(selection.members, staged)) {
      continue;
    }
    const bool same = path == staged;
    const bool suffix =
        path.size() > staged.size() + 1 &&
        path[path.size() - staged.size() - 1] == '/' &&
        path.compare(path.size() - staged.size(), staged.size(), staged) == 0;
    if (same || suffix) {
      return StagedMember{staged, ends_with(staged, FACADE_SUFFIX)};
    }
  }
  return std::nullopt;
}

std::vector<analyzer::PackagePolicy> std_package_policies(
    const StdSelection& selection) {
  std::vector<analyzer::PackagePolicy> policies;
  policies.reserve(selection.members.size());
  for (std::string_view member : selection.members) {
    const StdPackageSeals* const seals = std_seals_for(member);
    policies.push_back(analyzer::PackagePolicy{
        member,
        STD_SUITE_IDENTITY,
        seals == nullptr ? std::span<const std::string_view>{}
                         : std::span<const std::string_view>(
                               seals->suite_only,
                               static_cast<usize>(seals->suite_only_count)),
        {}});
  }
  return policies;
}

}  // namespace pipeline
