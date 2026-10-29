// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The token types the server classifies, in legend order: a token's type
// on the wire is its index in this array.
text::JsonValue semantic_token_types();

// An empty SemanticTokens result, for a document the server does not
// hold or one the frontend could not read.
text::JsonValue semantic_tokens_empty();

// The semantic tokens for a document, as an LSP SemanticTokens object.
// Keywords, literals, comments, and operators come from the lossless
// lexer; an identifier's kind comes from the checker's records, so a
// name is colored as what the compiler resolved it to.
text::JsonValue semantic_tokens(const Document& document,
                                const DocumentAnalysis& analysis);

// The tokens that start inside [start, end), as the same object. A
// client that shows one viewport asks for it instead of the whole
// buffer.
text::JsonValue semantic_tokens_range(const Document& document,
                                      const DocumentAnalysis& analysis,
                                      Position start,
                                      Position end);

// Remembers the last token array per document, so a delta request can
// answer the edit between the previous result and the current one
// instead of the whole array.
class SemanticTokensCache {
 public:
  // The whole array, remembered under a fresh result id.
  text::JsonValue full(const Document& document,
                       const DocumentAnalysis& analysis);

  // The edit from the result the client names, or the whole array when
  // the client's result is not the one remembered.
  text::JsonValue delta(const Document& document,
                        const DocumentAnalysis& analysis,
                        std::string_view previous_result_id);

  void erase(std::string_view uri);

 private:
  struct Entry {
    std::string result_id;
    std::vector<u32> data;
  };

  std::unordered_map<std::string, Entry> entries_;
  u64 next_id_ = 0;
};

}  // namespace lsp
