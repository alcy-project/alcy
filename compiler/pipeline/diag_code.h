// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace pipeline {

// The checks this component reports, counted from 1. The ids are its own:
// a component does not know how many components there are or what they
// are called, because `diag` turns (component, id) into the code a user
// reads. `I` is this component's letter; `diag/stage.h` has the
// rest and `compiler/docs/diagnostics.md` says what each check means.
//
// Every id is spelled out rather than left to count from the one above.
// A reader then sees a check's code without counting lines, and
// inserting a check in the middle shows up as a diff to every id after
// it instead of shifting them silently.
//
// Ids start at 1: a zero id is never a check that exists, which is the
// ambiguity an optional code would otherwise have to answer for. 255 is
// the ceiling, which is what the `u8` gives; a component near it wants
// its checks split, not a wider field.
enum class DiagCode : u8 {
  NoManifest = 1,
  IoError = 2,
  NotImplemented = 3,
  NoTargets = 4,
  LinkError = 5,
  // Moved here with the parse stage: the syntax arena belongs to the run
  // rather than to one file, so the check went to whoever drives the
  // files through it.
  SpanArenaExhausted = 6,
  UnknownSourceFile = 7,
  InvalidSourcePath = 8,
  // `compile foo` with no `-o`: a mode whose suffix is empty has nothing
  // to name the artifact with, so the caller has to.
  NoOutputName = 9,
  // Moved here from `pkg` with the selection: which files a manifest
  // makes modules is a question about what discovery found, so it belongs
  // to the module that ran it.
  InvalidModuleSelection = 10,
  UnselectedFile = 11,
  // The triple names a backend this build of the compiler does not
  // contain, so there is no layout to emit against.
  UnknownTarget = 12,
  // A path dependency resolves into a directory already being
  // resolved, so the closure would never end.
  DependencyCycle = 13,
  // A dependency's manifest name is not one module path
  // segment, so it cannot name the package root a `use`
  // spells.
  DependencyBadName = 14,
  // Two package roots would open under one spelling: two
  // dependencies behind one identity, a dependency named like
  // the package that loads it, or one named like a staged
  // standard-library member.
  DependencyIdentityClash = 15,
  // A suite specifier does not match the suite manifest it
  // points at: the owner or name differs, or the member it
  // names is not one the suite lists.
  DependencySuiteMismatch = 16,
  // The build's backend cannot write the requested kind of output: an
  // object-only backend asked for a module, or the reverse.
  UnsupportedOutput = 17,
  // A release build asked a backend that has no optimizer for optimized
  // code; the code would be silently unoptimized otherwise.
  NoOptimizer = 18,
  // The module writer failed. Objects have their own failure so the
  // message can name what was being written.
  CannotEmitModule = 19,
  // No backend was compiled in at all: the command can check, and a
  // build command says so here.
  NoBackend = 20,
  // A release build asked for an IR form, and no IR form is optimized;
  // the output would be silently unoptimized otherwise.
  IrNoOptimizer = 21,
};

}  // namespace pipeline
