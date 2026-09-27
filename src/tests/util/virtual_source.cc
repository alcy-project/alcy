// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tests/util/virtual_source.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "source/source.h"

namespace tests {

namespace {

// The extension is not part of a module name, so it comes off the source
// name here rather than in every caller.
constexpr std::string_view SOURCE_EXTENSION = ".al";

std::string_view module_name(std::string_view source_name) {
  if (source_name.size() > SOURCE_EXTENSION.size() &&
      source_name.substr(source_name.size() - SOURCE_EXTENSION.size()) ==
          SOURCE_EXTENSION) {
    return source_name.substr(0, source_name.size() - SOURCE_EXTENSION.size());
  }
  return source_name;
}

}  // namespace

SourceSet add_sources(source::SourceManager& sources,
                      std::initializer_list<VirtualSource> files,
                      std::string_view root) {
  SourceSet set;
  set.inputs.reserve(files.size());
  for (const VirtualSource& file : files) {
    const source::FileId id = sources.add_virtual(file.name, file.bytes);
    if (file.name == root) {
      // The entry module has no name, which is how the analyzer tells it
      // from every other module in the input.
      set.inputs.push_back({"", id});
      continue;
    }
    // Reserved before the view is taken, and never reallocated afterwards,
    // so the view stays valid however long the caller holds the set.
    set.names.emplace_back(module_name(file.name));
    set.inputs.push_back({set.names.back(), id});
  }
  return set;
}

}  // namespace tests
