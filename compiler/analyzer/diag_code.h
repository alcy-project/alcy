// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace analyzer {

// The checks this component reports, counted from 1. The ids are its own:
// a component does not know how many components there are or what they
// are called, because `diag` turns (component, id) into the code a user
// reads. `C` is this component's letter; `diag/stage.h` has the
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
  DuplicateModule = 1,
  UnresolvedImport = 2,
  AmbiguousImport = 3,
  UnreachableFile = 4,
  InvalidPath = 5,
  InvalidModuleTree = 6,
  SpanArenaExhausted = 7,
  RecursiveType = 8,
  UnknownType = 9,
  DuplicateDefinition = 10,
  ReservedName = 11,
  ArityMismatch = 12,
  GenericArguments = 13,
  UnsupportedType = 14,
  InvalidIr = 15,
  TypeMismatch = 16,
  UnknownValue = 17,
  ArityError = 18,
  InvalidOperation = 19,
  NonExhaustiveMatch = 20,
  RefutableLet = 21,
  MustUse = 22,
  BadQuestion = 23,
  BadReturn = 24,
  BadAssignment = 25,
  BreakOutsideLoop = 26,
  NotCompKnown = 27,
  InvalidComp = 28,
  UnknownIntrinsic = 29,
  BadDropSignature = 30,
  DropOnCopy = 31,
  TooDeep = 32,
};

}  // namespace analyzer
