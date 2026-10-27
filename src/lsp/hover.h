// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>

#include "lsp/document.h"

namespace lsp {

// The markdown hover for the declaration at position, or nullopt when
// nothing there answers. A struct or enum declaration shows its memory
// layout, a field or variant shows its place in it, and a function or a
// static shows the type the checker decided.
//
// The frontend runs once per call, and every layout and type is read
// from that one checked package.
std::optional<std::string> hover_markdown(const Document& document,
                                          Position position);

}  // namespace lsp
