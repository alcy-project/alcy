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
  RecursiveType = 7,
  UnknownType = 8,
  DuplicateDefinition = 9,
  ReservedName = 10,
  ArityMismatch = 11,
  GenericArguments = 12,
  UnsupportedType = 13,
  InvalidIr = 14,
  TypeMismatch = 15,
  UnknownValue = 16,
  ArityError = 17,
  InvalidOperation = 18,
  NonExhaustiveMatch = 19,
  RefutableLet = 20,
  MustUse = 21,
  BadQuestion = 22,
  BadReturn = 23,
  BadAssignment = 24,
  BreakOutsideLoop = 25,
  NotCompKnown = 26,
  InvalidComp = 27,
  UnknownIntrinsic = 28,
  BadDropSignature = 29,
  DropOnCopy = 30,
  TooDeep = 31,
  ArenaExhausted = 32,
  // A `use` or a qualified path reached a module a dependency's
  // `[modules] export` list does not name, so the boundary keeps it
  // to the package that declares it.
  ExportWithheld = 33,
  // A capture list reached checking with an entry that cannot be
  // captured, or the body used an outer local the list does not
  // name, or declared one it never uses.
  InvalidCapture = 34,
  // The shared name table a checker and a lowerer intern into was
  // sized for fewer names than this package declares.
  NameTableExhausted = 35,
  // An operation that needs the gate (ADR-0050) sits in safe code: a
  // call to an unsafe function or intrinsic, or a raw pointer
  // dereference, with no enclosing `unsafe { ... }` block.
  UnsafeOperation = 36,
  // An implementation of a spec its package seals to a suite
  // (ADR-0053) comes from outside that package and suite.
  SpecSealed = 37,
  // A package's manifest seals a name that is not a spec the package
  // declares, so the seal would cover nothing.
  SpecSealUnknown = 38,
  // A declared super-spec does not name a spec in scope, or names one
  // with arguments, which supers do not support yet.
  SpecSuperBadTarget = 39,
  // A chain of super-specs returns to where it started.
  SpecSuperCycle = 40,
  // An implementation of a spec has no implementation of one of its
  // super-specs for the same target.
  SpecSuperMissing = 41,
};

}  // namespace analyzer
