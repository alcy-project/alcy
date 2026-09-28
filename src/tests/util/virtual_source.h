// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "fpag/base/numeric.h"
#include "source/source.h"

namespace tests {

// A source file a test has as text rather than as a file on disk.
struct VirtualSource {
  // Path-like name. The analyzer derives a module name from it, so
  // "util/vec.al" and "main.al" name modules the way files do.
  std::string_view name;
  std::string_view bytes;
};

// The sources one case declared. It stands in for the scratch directory a
// case used to create, and keeps the `dir` name at the call site so the
// cases read the way they were written.
class DeclaredSources {
 public:
  void add(std::string_view name, std::string_view bytes) {
    files_.push_back({name, bytes});
  }

  // The declaration `name` refers to, or nullptr when the case never
  // declared one. A case that lists a name it did not declare used to fail
  // its setup, so a nullptr here is that same condition.
  const VirtualSource* find(std::string_view name) const {
    for (const VirtualSource& file : files_) {
      if (file.name == name) {
        return &file;
      }
    }
    return nullptr;
  }

  usize size() const { return files_.size(); }

 private:
  std::vector<VirtualSource> files_;
};

// The module name a source name gives: the path with the extension off,
// so "util/vec.al" names the module "util/vec" and "main.al" names the
// empty name the entry module carries.
std::string module_name(std::string_view source_name);

// Module inputs, plus the storage their names borrow.
//
// `analyzer::ModuleInput` holds its name as a view, so returning a bare
// vector of them would leave every name pointing at a temporary. A
// caller keeps the set for as long as it uses the inputs, and the names
// live exactly as long.
struct SourceSet {
  // A deque, not a vector: the inputs borrow from these, and appending to
  // a vector would move every string already in it, which for a name short
  // enough to live inside the string object leaves a view pointing into
  // freed memory.
  std::deque<std::string> names;
  std::vector<analyzer::ModuleInput> inputs;
};

// Registers each source in `sources` under its name and returns the module
// inputs the analyzer takes, with the source named `root` as the entry
// module.
SourceSet add_sources(source::SourceManager& sources,
                      std::initializer_list<VirtualSource> files,
                      std::string_view root);

// Registers the source `name` refers to and returns the module input it
// makes, with `is_root` naming it as the entry module. std::nullopt means
// the case never declared a source by that name.
//
// A name the case did declare is registered under its own name, not under
// the name the caller passed, so two files claiming one module name stay
// two distinct sources.
std::optional<analyzer::ModuleInput> register_source(
    source::SourceManager& sources,
    const DeclaredSources& declared,
    std::string_view name,
    bool is_root,
    std::deque<std::string>& name_storage);

}  // namespace tests
