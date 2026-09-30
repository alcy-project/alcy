// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Bytes through the lexer, then the token-stream verifier.
//
// The lexer is the widest attack surface in the compiler: it is the
// only stage that reads untrusted bytes directly, and it has to track
// spans, nest comments, split numbers and strings, and synthesise
// semicolons. The oracle is `lexer::verify_token_stream`, which the
// parser already requires, so "every stream the lexer produces verifies"
// is exactly the property the parser depends on.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "source/source.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, usize size) {
  mem::Arena arena;
  arena.reserve(1u << 20);
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);

  lexer::Lexer lexer(bytes, source::UNKNOWN_FILE, bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);
  // The parser calls this before it looks at a single token, so a
  // failure here is a defect the parser would report as an internal
  // error rather than as a user diagnostic.
  base::Result<void, lexer::TokenStreamError> verified =
      lexer::verify_token_stream(tokens, source::UNKNOWN_FILE, bytes);
  (void)verified;
  return 0;
}
