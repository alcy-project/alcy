// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/printer.h"

#include <string>
#include <string_view>
#include <vector>

#include "cst/cst.h"
#include "fmt/doc.h"
#include "fmt/formatter.h"

namespace alcy_fmt {

Printer::Printer(std::string_view source, const FormatConfig& config)
    : source_(source), config_(config) {}

Doc Printer::run(const cst::Node& root) {
  const std::vector<Chunk> chunks = collect_chunks(root, cst::BraceRole::Block);
  if (chunks.empty()) {
    return doc_empty();
  }
  return print_body_lines(chunks, cst::BraceRole::Block, true);
}

}  // namespace alcy_fmt
