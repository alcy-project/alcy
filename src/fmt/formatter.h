// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"

namespace alcy_fmt {

// The layout parameters. They are injected rather than stored: the
// formatter holds no configuration of its own, so one input lays out the
// same way wherever it is run.
struct FormatConfig {
  // The column the renderer aims to keep lines within. A line may still
  // exceed it when a single unbreakable run does.
  usize line_length = 100;
  // Spaces one nesting level adds.
  usize indent_width = 2;
};

// The one configuration the tool uses. A host may inject another, but
// nothing reads one from a file or the environment.
constexpr FormatConfig default_format_config() {
  return FormatConfig{};
}

// Formats source and returns the canonical layout. The result is
// idempotent and is defined for every input, including one the grammar
// rejects.
std::string format(std::string_view source, const FormatConfig& config);

}  // namespace alcy_fmt
