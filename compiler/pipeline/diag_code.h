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
};

}  // namespace pipeline
