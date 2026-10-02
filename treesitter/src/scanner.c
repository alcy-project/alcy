// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The external scanner for the alcy grammar.
//
// Three tokens live here, and none of them can be written as a regular
// expression:
//
//   _block_lbrace   the `{` that opens a block whose header it belongs to,
//                   which the specification requires to stay on that line
//   _newline        the `;` the lexer inserts when a line ends a statement
//   block_comment   a `/* */` comment, which nests
//
// The rule for a newline is split in two on purpose. Whether a newline may
// end a statement at all is decided by where `_newline` appears in
// grammar.js, and whether the next token continues the construct is decided
// here. That is why there is no copy of the lexer's list of
// statement-ending tokens: the grammar already states it, and a second copy
// is a second thing to forget to update.
//
// Neither token steps over a comment. A comment is an extra, so declining to
// produce a token when one is in the way hands it to the internal lexer,
// which keeps it in the tree, and the scanner is asked again once it is
// gone. A consequence worth knowing: the newline that terminates a statement
// is the one after the last comment on the line, which is the same `;` the
// compiler's lexer arrives at.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum TokenType {
  BLOCK_LBRACE,
  NEWLINE,
  BLOCK_COMMENT,
  ERROR_SENTINEL,
};

typedef struct {
  // Set when a newline stood between a block's header and its brace, and
  // kept until a brace is finally accepted as one. It has to be kept: this
  // scanner is asked again once the internal lexer has taken the newline as
  // whitespace, and by then the newline is no longer in front of it.
  bool newline_before_brace;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// The characters the lexer steps over without ending a line, which is
// exactly the set it treats as such.
static inline bool is_horizontal_space(int32_t c) {
  return c == ' ' || c == '\t' || c == '\r';
}

// Steps over the spaces between a token and whatever the parser needs next,
// and reports whether a newline was among them.
static bool skip_to_next_line(TSLexer *lexer) {
  while (is_horizontal_space(lexer->lookahead)) {
    skip(lexer);
  }
  if (lexer->lookahead != '\n') {
    return false;
  }
  while (is_horizontal_space(lexer->lookahead) || lexer->lookahead == '\n') {
    skip(lexer);
  }
  return true;
}

// Whether the token at the cursor continues the construct the line above it
// began, which makes the newline between them whitespace rather than a
// statement terminator: a separator, a closing delimiter, a field or method
// access, or the end of the file.
//
// The lexer's set also holds `else`, and nothing here has to: `else` can
// only follow the `}` of a block, and `}` already suppresses the newline
// before it. That is worth more than the symmetry, because recognising the
// keyword means reading ahead, and a scanner that reads ahead and then
// declines leaves the lexer in a state it did not mean to leave it in.
static bool continues_after_newline(TSLexer *lexer) {
  if (lexer->eof(lexer)) {
    return true;
  }
  switch (lexer->lookahead) {
    case ',':
    case ')':
    case ']':
    case '}':
    case '.':
      return true;
    default:
      return false;
  }
}

// A comment, of either nesting or not. Only `/* */` nests.
static bool skip_block_comment(TSLexer *lexer) {
  int32_t depth = 1;
  for (;;) {
    if (lexer->eof(lexer)) {
      // Unterminated. The internal lexer reports it, which is what the
      // compiler's lexer does too.
      return false;
    }
    if (lexer->lookahead == '*') {
      advance(lexer);
      if (lexer->lookahead == '/') {
        advance(lexer);
        if (--depth == 0) {
          return true;
        }
      }
      continue;
    }
    if (lexer->lookahead == '/') {
      advance(lexer);
      if (lexer->lookahead == '*') {
        advance(lexer);
        ++depth;
      }
      continue;
    }
    advance(lexer);
  }
}

static bool scan_block_comment(TSLexer *lexer) {
  if (lexer->lookahead != '/') {
    return false;
  }
  advance(lexer);
  if (lexer->lookahead != '*') {
    return false;
  }
  advance(lexer);
  if (!skip_block_comment(lexer)) {
    return false;
  }
  lexer->result_symbol = BLOCK_COMMENT;
  lexer->mark_end(lexer);
  return true;
}

// The brace that opens a block, which the specification requires to be on
// its header line.
static bool scan_block_lbrace(Scanner *scanner, TSLexer *lexer) {
  if (scanner->newline_before_brace) {
    return false;
  }
  if (skip_to_next_line(lexer)) {
    scanner->newline_before_brace = true;
    return false;
  }
  if (lexer->lookahead != '{') {
    return false;
  }
  advance(lexer);
  lexer->mark_end(lexer);
  lexer->result_symbol = BLOCK_LBRACE;
  scanner->newline_before_brace = false;
  return true;
}

static bool scan_newline(TSLexer *lexer) {
  if (!skip_to_next_line(lexer)) {
    return false;
  }
  if (continues_after_newline(lexer)) {
    return false;
  }
  lexer->mark_end(lexer);
  lexer->result_symbol = NEWLINE;
  return true;
}

void *tree_sitter_alcy_external_scanner_create(void) {
  static Scanner scanner = {false};
  return &scanner;
}

void tree_sitter_alcy_external_scanner_destroy(void *payload) { (void)payload; }

unsigned tree_sitter_alcy_external_scanner_serialize(void *payload,
                                                      char *buffer) {
  const Scanner *scanner = (const Scanner *)payload;
  buffer[0] = scanner->newline_before_brace ? 1 : 0;
  return 1;
}

void tree_sitter_alcy_external_scanner_deserialize(void *payload,
                                                    const char *buffer,
                                                    unsigned length) {
  Scanner *scanner = (Scanner *)payload;
  scanner->newline_before_brace = length > 0 && buffer[0] != 0;
}

bool tree_sitter_alcy_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
  Scanner *scanner = (Scanner *)payload;

  if (!valid_symbols[BLOCK_LBRACE]) {
    // The header is over, so a newline that held one back no longer holds
    // anything back, and a later block is free to open its brace on its own
    // line again.
    scanner->newline_before_brace = false;
  } else if (scan_block_lbrace(scanner, lexer)) {
    return true;
  }

  if (valid_symbols[BLOCK_COMMENT] && scan_block_comment(lexer)) {
    return true;
  }
  if (valid_symbols[NEWLINE] && scan_newline(lexer)) {
    return true;
  }
  return false;
}
