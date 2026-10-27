// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/deserialize.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

// One path dependency, loaded from source: the manifest it declares
// and the modules that manifest selects, named by their paths within
// the package, plus the files discovery loaded for them. `suite` is
// the suite the specifier that reached it names ("owner/name"), empty
// for a package no suite holds; the view borrows the context arena.
// The manifest views borrow the context arena too, which outlives the
// result.
struct LoadedDependency {
  pkg::PackageManifest manifest;
  std::string_view suite;
  std::vector<analyzer::ModuleInput> modules;
  std::vector<source::FileId> files;
  // A package resolved from a summary carries the artifact (ADR-0078);
  // its syntax is spelled after the tree's own sources are parsed, when
  // the arena can still hand out nodes.
  std::optional<ir::LoadedIr> artifact;
  // The module names above, and the object the link needs when the
  // summary answered for the sources: both borrow the run's interner,
  // so they outlive the tree that names them.
  std::vector<std::string_view> module_names;
  std::string_view object;
};

// The dependencies one run has staged, keyed by the directory they were
// read from. A suite build resolves every member's dependencies and the
// members share theirs, so a directory is read, hashed, and deserialized
// once per run. Element addresses stay stable: a closure holds pointers
// into the table while later resolutions append to it.
struct StagedDependencies {
  std::deque<LoadedDependency> entries;
  // One directory's contribution to a closure: its own entry followed by
  // what its dependencies brought, in traversal order. A resolution that
  // reaches the directory again takes this instead of reading it.
  std::unordered_map<std::string, std::vector<const LoadedDependency*>> by_dir;
};

// The dependencies one resolution reached, in traversal order. An entry
// lives in the run's table rather than in the closure, so two
// resolutions that share a dependency share its entry - and its sources
// are read once.
class DependencyClosure {
 public:
  DependencyClosure() = default;
  explicit DependencyClosure(std::vector<const LoadedDependency*> entries)
      : entries_(std::move(entries)) {}

  class Iterator {
   public:
    explicit Iterator(const LoadedDependency* const* at) : at_(at) {}
    const LoadedDependency& operator*() const { return **at_; }
    Iterator& operator++() {
      ++at_;
      return *this;
    }
    bool operator!=(const Iterator& other) const { return at_ != other.at_; }

   private:
    const LoadedDependency* const* at_;
  };

  Iterator begin() const { return Iterator(entries_.data()); }
  Iterator end() const { return Iterator(entries_.data() + entries_.size()); }
  [[nodiscard]] usize size() const { return entries_.size(); }
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  const LoadedDependency& operator[](usize i) const { return *entries_[i]; }

 private:
  std::vector<const LoadedDependency*> entries_;
};

// The identity a package's manifest name gives: the name a `use`
// spells, with `-` normalized to `_` the way the modules chapter
// describes.
inline std::string package_identity(std::string_view name) {
  std::string identity(name);
  for (char& c : identity) {
    if (c == '-') {
      c = '_';
    }
  }
  return identity;
}

// Resolves the path dependencies of the package at `root`:
// each dependency's manifest is read from the directory its
// `path` names - a package manifest straight into the loader,
// a suite manifest through its member list - and a directory
// already being resolved is a cycle. Every dependency declares
// a target the way the package it belongs to does, and its name
// must be one module path segment, because it becomes the
// package root a `use` spells. Standard-library members are not
// loaded here: they stage from the embedded suite, and a
// dependency that names one adds it to the selection the caller
// merges from every manifest the closure holds.
base::Result<DependencyClosure, diag::Reported> resolve_dependencies(
    PipelineContext& ctx,
    const path::Path& root,
    const pkg::PackageManifest& manifest);

}  // namespace pipeline
