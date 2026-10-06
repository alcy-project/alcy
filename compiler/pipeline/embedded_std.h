// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "analyzer/resolve.h"
#include "fpag/base/numeric.h"

namespace pipeline {

// One embedded standard library source, named by its suite-relative
// path (`core/prelude.al`). Attached as a prelude module under that name,
// so compilations need no install-layout assumptions and no filesystem
// is touched. The table is generated; see tools/embed_std.py.
struct StagedSource {
  const char* path;
  const u8* data;
  u64 len;
};

extern const StagedSource STAGED_SOURCES[];
extern const usize STAGED_SOURCE_COUNT;

// One member of the embedded suite and the members it depends on, by
// member name. Generated from the package manifests, which stay the
// single source of truth; see tools/embed_std.py.
struct StdPackageDeps {
  const char* name;
  const char* const* deps;
  u64 dep_count;
};

extern const StdPackageDeps STD_PACKAGE_DEPS[];
extern const usize STD_PACKAGE_COUNT;

// The specs each member seals to the standard library's suite
// (`[spec] suite-only` in its manifest), by member name. Generated the
// way the dependency table is, and for the same reason: the compiler
// never parses a staged member's manifest, so this table is where its
// seal is read (ADR-0053); see tools/embed_std.py.
struct StdPackageSeals {
  const char* name;
  const std::string_view* suite_only;
  u64 suite_only_count;
};

extern const StdPackageSeals STD_PACKAGE_SEALS[];
extern const usize STD_PACKAGE_SEAL_COUNT;

// The embedded suite's identity, the spelling a suite specifier uses.
inline constexpr std::string_view STD_SUITE_IDENTITY = "alcy/std";

// The seals the embedded member `name` declares, or nullptr for a name
// that is no member.
inline const StdPackageSeals* std_seals_for(std::string_view name) {
  for (usize i = 0; i < STD_PACKAGE_SEAL_COUNT; ++i) {
    if (name == STD_PACKAGE_SEALS[i].name) {
      return &STD_PACKAGE_SEALS[i];
    }
  }
  return nullptr;
}

// Every public name each member carries, for the missing-dependency
// hint: an unresolved name found here names the package to add.
// Declared with the analyzer's hint type so the generated table is
// defined once and shared; see tools/embed_std.py.
extern const analyzer::StdHint STD_HINTS[];
extern const usize STD_HINT_COUNT;

// The hint table as a span. The generated arrays carry their count
// beside them, so the span is built here rather than at each use.
inline std::span<const analyzer::StdHint> std_hints() {
  return {STD_HINTS, STD_HINT_COUNT};
}

}  // namespace pipeline
