// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "source/source.h"

namespace tests {

// A source file a test has as text rather than as a file on disk.
struct VirtualSource {
  // Path-like name. The analyzer derives a module name from it, so
  // "util/vec.al" and "main.al" name modules the way files do.
  std::string_view name;
  std::string_view bytes;
};

// Module inputs, plus the storage their names borrow.
//
// `analyzer::ModuleInput` holds its name as a view, so returning a bare
// vector of them would leave every name pointing at a temporary. A
// caller keeps the set for as long as it uses the inputs, and the names
// live exactly as long.
struct SourceSet {
  std::vector<std::string> names;
  std::vector<analyzer::ModuleInput> inputs;
};

// Registers each source in `sources` under its name and returns the module
// inputs the analyzer takes, with the source named `root` as the entry
// module.
//
// The name a module gets is its source name without the extension, so a
// test that used to write "util/vec.al" and pass "util/vec" passes the
// same pair here and gets the same module.
SourceSet add_sources(source::SourceManager& sources,
                      std::initializer_list<VirtualSource> files,
                      std::string_view root);

}  // namespace tests
