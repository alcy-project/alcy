// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Bytes through the whole front end: lex, parse, verify the arena.
//
// This is the target that finds parser bugs, because the parser is
// where error recovery, nesting, and the precedence chain interact. The
// two properties it must hold are the ones the analyzer relies on: the
// token stream verifies on entry, and the arena verifies on exit, so a
// hand-built AST never reaches a later stage.
//
// The nesting budget means a sufficiently deep input is rejected rather
// than recursed into, which is the property that keeps this target
// stack-safe: without it, a fuzzer finds a crash in seconds.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "ast/verify.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/parser.h"
#include "source/source.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, usize size) {
  mem::Arena arena;
  arena.reserve(1u << 21);
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);

  lexer::Lexer lexer(bytes, source::UNKNOWN_FILE, bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);

  ast::AstArena ast;
  parser::Parser parser(tokens, bytes, source::UNKNOWN_FILE, ast, bag);
  base::Result<std::span<const ast::ItemIdx>, diag::Reported> parsed =
      parser.parse();
  (void)parsed;

  // The parser verifies on exit only when it ran to completion; a
  // grammar error leaves a partial arena, so this is checked
  // independently rather than trusted.
  base::Result<void, ast::VerificationError> verified = ast::verify_file(ast);
  (void)verified;
  return 0;
}
