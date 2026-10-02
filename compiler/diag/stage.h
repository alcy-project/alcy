// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace diag {

// Which part of the compiler a diagnostic came from, and the one letter
// that says so in the code a user reads.
//
// A letter, not a number, because the numbering used to be a stride per
// stage of the pipeline and a stride is a claim about the whole compiler
// that a stage has no business making. `lexer` picking 2000 means
// `lexer` knows there are eight stages and which order they run in; a
// letter means it knows only its own.
//
// The order here is the build flow, which is also the order the letters
// were handed out in, and it is not alphabetical: `A` is the lexer
// because a stage that gains its first error should not have to
// renumber, and the lexer will, when UTF-8 validation arrives.
//
// This is an implementation fact and no part of it is a language
// property, so it belongs to `compiler/docs/` and not to `docs/spec/`.
// A program cannot observe which stage rejected it, only what it was
// told.
//
// `CodegenLlvm` and `CodegenNative` are two implementations of one
// pipeline position rather than two positions. A run has exactly one of
// them. They are separate because their failure modes are: a code
// search that answered for both would answer for neither.
enum class Stage : u8 {
  Lexer,
  Parser,
  Analyzer,
  Lowering,
  Borrow,
  Ir,
  Pkg,
  Pipeline,
  CodegenLlvm,
  CodegenNative,
};

// A code, as the pair it actually is: which component, and which of its
// checks. A component counts its own ids from 1, so a Code is only
// meaningful with its stage, and that is why there is no number here
// that a reader could decode on its own.
//
// Ids start at 1 rather than 0 so that a zero id is never a check that
// exists, which is the ambiguity an optional code would otherwise have
// to answer for. A component has at most 255 checks and the widest has
// 32, so the id is a `u8`; a component that reaches the ceiling wants its
// checks split, not a wider field. The rendered width is three digits so
// that codes sort in the order they were assigned, which is the order a
// bug report lists them in.
struct Code {
  Stage stage = Stage::Lexer;
  u8 id = 0;

  // Two codes are the same check when both halves are, which is what lets
  // a case say `code == Code{Stage::Lexer, 7}` and mean what a rendered
  // `EL007` means.
  friend bool operator==(const Code&, const Code&) = default;
};

// The letter a stage is written as. Assigned once and never reassigned:
// a letter is retired with its component, and a code quoted in a bug
// report has to mean the same thing for as long as the report can
// survive. `E`, `N` and `W` are not in the table because they are the
// severity letters, which sit beside this one in the same bracket, and
// overlapping the two would produce `EE`.
char stage_letter(Stage stage);

}  // namespace diag
