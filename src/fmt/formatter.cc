// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/formatter.h"

#include <string>
#include <string_view>
#include <vector>

#include "cst/cst.h"
#include "cst/lex.h"
#include "fmt/doc.h"
#include "fmt/printer.h"
#include "fpag/base/numeric.h"

namespace alcy_fmt {

std::string format(std::string_view source, const FormatConfig& config) {
  const std::vector<cst::Lexeme> lexemes = cst::lex(source);
  const cst::Node root = cst::build_cst(lexemes);
  Printer printer(source, config);
  const Doc document = printer.run(root);
  // The output carries the source's bytes in their tokens plus the
  // indentation the layout adds, so the renderer starts a little above
  // the source's size rather than growing into it.
  std::string out =
      render(document, config.line_length, source.size() + source.size() / 8);
  while (!out.empty() &&
         (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) {
    out.pop_back();
  }
  if (!out.empty()) {
    out.push_back('\n');
  }
  return out;
}

}  // namespace alcy_fmt
