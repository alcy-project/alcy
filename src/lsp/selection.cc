// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/selection.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "cst/lex.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

bool contains(diag::Span outer, diag::Span inner) {
  return outer.offset <= inner.offset &&
         outer.offset + outer.length >= inner.offset + inner.length;
}

template <typename Nodes>
void gather(const Nodes& nodes,
            source::FileId root,
            std::vector<diag::Span>* out) {
  nodes.for_each_node([&](const auto index) {
    const diag::Span span = nodes[index].span;
    if (span.file == root && span.length != 0) {
      out->push_back(span);
    }
  });
}

}  // namespace

text::JsonValue selection_ranges(const Document& document,
                                 const DocumentAnalysis& analysis,
                                 std::span<const Position> positions) {
  const std::string_view text = document.text();
  const source::FileId root = analysis.root();

  // Every node span in the buffer, gathered once and reused for each
  // requested position.
  std::vector<diag::Span> nodes;
  if (analysis.ok()) {
    const ast::AstArena& ast = analysis.context().ast;
    gather(ast.items, root, &nodes);
    gather(ast.types, root, &nodes);
    gather(ast.exprs, root, &nodes);
    gather(ast.patterns, root, &nodes);
    gather(ast.stmts, root, &nodes);
    gather(ast.blocks, root, &nodes);
    gather(ast.paths, root, &nodes);
  }
  const std::vector<cst::Lexeme> lexemes = cst::lex(text);

  diag::Span whole;
  whole.file = root;
  whole.length = static_cast<u32>(text.size());

  text::JsonValue result = text::JsonValue::make_array();
  for (const Position position : positions) {
    const usize offset = document.index().byte_offset(text, position);
    std::vector<diag::Span> spans;

    for (const cst::Lexeme& lexeme : lexemes) {
      if (lexeme.kind == cst::Lexeme::Kind::End) {
        break;
      }
      if (lexeme.offset <= offset && offset < lexeme.offset + lexeme.length) {
        diag::Span word;
        word.file = root;
        word.offset = lexeme.offset;
        word.length = lexeme.length;
        spans.push_back(word);
        break;
      }
    }

    const usize line_begin =
        document.index().byte_offset(text, Position{position.line, 0});
    const usize next_line =
        document.index().byte_offset(text, Position{position.line + 1, 0});
    usize line_end = next_line;
    while (line_end > line_begin &&
           (text[line_end - 1] == '\n' || text[line_end - 1] == '\r')) {
      --line_end;
    }
    diag::Span line;
    line.file = root;
    line.offset = static_cast<u32>(line_begin);
    line.length = static_cast<u32>(line_end - line_begin);
    spans.push_back(line);

    for (const diag::Span node : nodes) {
      if (node.offset <= offset &&
          offset < static_cast<usize>(node.offset) + node.length) {
        spans.push_back(node);
      }
    }
    spans.push_back(whole);

    std::sort(spans.begin(), spans.end(),
              [](diag::Span left, diag::Span right) {
                return left.length < right.length;
              });
    std::vector<diag::Span> chain;
    for (const diag::Span span : spans) {
      if (chain.empty()) {
        if (span.length != 0) {
          chain.push_back(span);
        }
        continue;
      }
      if (chain.back().offset == span.offset &&
          chain.back().length == span.length) {
        continue;
      }
      if (contains(span, chain.back())) {
        chain.push_back(span);
      }
    }

    bool has_parent = false;
    text::JsonValue node;
    for (usize i = chain.size(); i-- > 0;) {
      text::JsonValue current = text::JsonValue::make_object();
      current.set("range",
                  range_json(document, chain[i].offset, chain[i].length));
      if (has_parent) {
        current.set("parent", std::move(node));
      }
      node = std::move(current);
      has_parent = true;
    }
    if (has_parent) {
      result.push(std::move(node));
    } else {
      result.push(text::JsonValue::make_object());
    }
  }
  return result;
}

}  // namespace lsp
