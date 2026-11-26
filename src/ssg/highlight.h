// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "fpag/base/result.h"

namespace ssg {

// The build-time highlighter: the same rendering as the playground's
// editor, loaded against the native tree-sitter runtime instead of
// the browser binding. Turning grammar captures into CSS classes and
// classes into runs of HTML is the shared logic; the difference is
// only the parser binding and where its sources live.

class Highlight {
 public:
  Highlight();
  ~Highlight();
  Highlight(Highlight&& other) noexcept;
  Highlight& operator=(Highlight&& other) noexcept;

  // Renders the HTML for a block of Alcy source: one
  // `<span class="hl-...">` run per highlight, HTML-escaped otherwise.
  std::string highlight_code(std::string_view code) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend base::Result<Highlight, std::string> create_highlighter(
      const std::string& highlights_path);
};

// Creates a highlighter over the in-process Alcy grammar. The query
// is read from `highlights_path`; the parse table is the compiled
// grammar linked into the generator.
base::Result<Highlight, std::string> create_highlighter(
    const std::string& highlights_path);

}  // namespace ssg
