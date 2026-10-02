// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstddef>
#include <span>

#include "analyzer/resolve.h"
#include "fpag/base/numeric.h"

namespace pipeline {

// One embedded standard library source, named by its suite-relative
// path (`core/prelude.al`). Attached as a prelude module under that name,
// so compilations need no install-layout assumptions and no filesystem
// is touched. The table is generated; see tools/embed_std.py.
struct StagedSource {
  const char* path;
  const unsigned char* data;
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
