// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/stage.h"

#include "debug/fatal.h"
#include "fpag/base/numeric.h"

namespace diag {

namespace {

// Indexed by Stage, not by letter: the two severity letters that are
// skipped leave gaps, and a table keyed by the letter would have to know
// which gaps are which.
constexpr char LETTERS[] = {
    'A',  // Lexer
    'B',  // Parser
    'C',  // Analyzer
    'D',  // Lowering
    'F',  // Borrow
    'G',  // Ir
    'H',  // Pkg
    'I',  // Pipeline
    'J',  // CodegenLlvm
    'K',  // CodegenNative
};

}  // namespace

char stage_letter(Stage stage) {
  const usize index = static_cast<usize>(stage);
  if (index >= sizeof(LETTERS)) {
    UNREACHABLE();
  }
  return LETTERS[index];
}

}  // namespace diag
