// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fpag/base/numeric.h"

namespace alcy_fmt {

// A layout document: the description of a formatted region before it is
// given a width. A renderer walks it once and chooses a flat or a broken
// rendering for each group, so the same document can be laid out for any
// line budget.
enum class DocKind : u8 {
  // Nothing.
  Empty,
  // Literal bytes.
  Text,
  // Every child, in order.
  Concat,
  // A space when flat and a newline when broken.
  Line,
  // Nothing when flat and a newline when broken. Used beside a
  // delimiter that should not carry a space when the region is flat.
  SoftLine,
  // Always a newline. A group holding one is broken.
  HardLine,
  // The child, indented by this node's indent when broken.
  Nest,
  // The child, rendered flat when it fits the remaining width and
  // broken otherwise.
  Group,
  // The first child when broken, the second when flat.
  IfBreak,
};

// One node of a layout document. kids holds the parts of a Concat, the
// single part of a Nest or a Group, or the broken/flat pair of an
// IfBreak.
struct Doc {
  DocKind kind = DocKind::Empty;
  std::string text;
  std::vector<Doc> kids;
  usize indent = 0;
};

Doc doc_empty();
Doc doc_text(std::string_view text);
Doc doc_concat(std::vector<Doc> parts);
Doc doc_line();
Doc doc_softline();
Doc doc_hardline();
Doc doc_nest(usize indent, Doc child);
Doc doc_group(Doc child);
Doc doc_if_break(Doc broken, Doc flat);

// Renders a document to bytes. width is the column the renderer aims to
// keep lines within; a line may still exceed it when a single
// unbreakable run does.
std::string render(const Doc& doc, usize width);

}  // namespace alcy_fmt
