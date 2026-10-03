// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pipeline {

// One path dependency, loaded from source: the manifest it declares
// and the modules that manifest selects, named by their paths within
// the package, plus the files discovery loaded for them. The manifest
// views borrow the context arena, which outlives the result.
struct LoadedDependency {
  pkg::PackageManifest manifest;
  std::vector<analyzer::ModuleInput> modules;
  std::vector<source::FileId> files;
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

// Resolves the path dependencies of the package at `root`: each
// dependency's manifest is read from the directory its `path` names,
// its own path dependencies load the same way, and a directory
// already being resolved is a cycle. Every dependency declares a
// target the way the package it belongs to does, and its name must
// be one module path segment, because it becomes the package root a
// `use` spells. Standard-library members are not loaded here: they
// stage from the embedded suite, and a dependency that names one
// adds it to the selection the caller merges from every manifest the
// closure holds.
base::Result<std::vector<LoadedDependency>, diag::Reported>
resolve_dependencies(PipelineContext& ctx,
                     const path::Path& root,
                     const pkg::PackageManifest& manifest);

}  // namespace pipeline
