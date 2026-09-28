// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/std_select.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/result.h"
#include "pipeline/embedded_std.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"

namespace pipeline {

namespace {

// The embedded suite this selection resolves against. Only `alcy/std`
// is embedded; anything else needs a registry the compiler does not
// have yet.
constexpr std::string_view STD_OWNER = "alcy";
constexpr std::string_view STD_SUITE = "std";

const StdPackageDeps* find_member(std::string_view name) {
  for (usize i = 0; i < STD_PACKAGE_COUNT; ++i) {
    if (STD_PACKAGE_DEPS[i].name == name) {
      return &STD_PACKAGE_DEPS[i];
    }
  }
  return nullptr;
}

bool selected(const std::vector<std::string_view>& members,
              std::string_view name) {
  for (std::string_view member : members) {
    if (member == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

base::Result<StdSelection, diag::Reported> resolve_std_selection(
    std::span<const pkg::Dependency> deps,
    diag::DiagBag& bag) {
  StdSelection selection;
  bool globbed = false;
  std::string_view glob_spec;
  for (const pkg::Dependency& dep : deps) {
    // A bare name is a local directory, resolved by inclusion through
    // pkg rather than selected here.
    if (dep.owner.empty()) {
      continue;
    }
    const bool embedded =
        dep.owner == STD_OWNER && (dep.suite.empty() || dep.suite == STD_SUITE);
    if (!embedded) {
      if (dep.source == pkg::DependencySource::Path) {
        continue;
      }
      if (dep.source == pkg::DependencySource::Unspecified) {
        const u32 index = bag.emit(
            diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
            "dependency '{}' names no source; give one of path, version, "
            "git",
            dep.spec);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      const u32 index = bag.emit(
          diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
          "dependency '{}' needs a fetcher the resolver does not have yet",
          dep.spec);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    // The standard library is embedded, so a source beside the name is
    // always a mistake rather than a pin.
    if (dep.source != pkg::DependencySource::Unspecified) {
      const u32 index =
          bag.emit(diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
                   "dependency '{}' is part of the embedded standard library, "
                   "which takes no source",
                   dep.spec);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    if (dep.suite.empty()) {
      // Two segments name a package of an owner. Inside the embedded
      // suite the member form is three segments, so `alcy/std` and
      // `alcy/core` are misspellings with an obvious fix each.
      if (dep.owner == STD_OWNER) {
        if (dep.member == STD_SUITE) {
          const u32 index = bag.emit(
              diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
              "'alcy/std' is a suite, not a package; write \"alcy/std/*\" "
              "for every member or \"alcy/std/<package>\" for one");
          (void)index;
          return base::make_err(diag::Reported{});
        }
        if (find_member(dep.member) != nullptr) {
          const u32 index =
              bag.emit(diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
                       "dependency '{}' is a member of alcy/std; write "
                       "\"alcy/std/{}\"",
                       dep.spec, dep.member);
          (void)index;
          return base::make_err(diag::Reported{});
        }
      }
      if (dep.source == pkg::DependencySource::Path) {
        continue;
      }
      if (dep.source == pkg::DependencySource::Unspecified) {
        const u32 index = bag.emit(
            diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
            "dependency '{}' names no source; give one of path, version, "
            "git",
            dep.spec);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      const u32 index = bag.emit(
          diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
          "dependency '{}' needs a fetcher the resolver does not have yet",
          dep.spec);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    if (dep.suite_glob) {
      globbed = true;
      glob_spec = dep.spec;
      continue;
    }
    if (find_member(dep.member) == nullptr) {
      const u32 index =
          bag.emit(diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
                   "dependency '{}' is not a member of alcy/std", dep.spec);
      (void)index;
      return base::make_err(diag::Reported{});
    }
    if (!selected(selection.members, dep.member)) {
      selection.members.push_back(dep.member);
    }
  }
  if (globbed) {
    for (usize i = 0; i < STD_PACKAGE_COUNT; ++i) {
      std::string_view member(STD_PACKAGE_DEPS[i].name);
      if (selected(selection.members, member)) {
        // A glob overlapping a named member would stage it twice over:
        // one spelling has to go.
        const u32 index = bag.emit(
            diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
            "dependency '{}' overlaps '{}'; name every member or glob the "
            "suite, not both",
            glob_spec, member);
        (void)index;
        return base::make_err(diag::Reported{});
      }
      selection.members.push_back(member);
    }
  }
  // Closure is explicit: a selected member's dependencies must be
  // selected too, never pulled silently.
  for (std::string_view member : selection.members) {
    const StdPackageDeps* info = find_member(member);
    for (u64 i = 0; i < info->dep_count; ++i) {
      std::string_view need(info->deps[i]);
      if (!selected(selection.members, need)) {
        const u32 index = bag.emit(
            diag::Severity::Error, PIPELINE_NOT_IMPLEMENTED,
            "alcy/std/{} requires alcy/std/{}; add it to [dependencies]",
            member, need);
        (void)index;
        return base::make_err(diag::Reported{});
      }
    }
  }
  return base::make_ok(std::move(selection));
}

}  // namespace pipeline
