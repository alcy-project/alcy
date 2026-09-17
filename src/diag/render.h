// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>

#include "diag/diagnostic.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"

namespace diag {

// Zero-copy source lookup for the renderer: both views borrow from storage
// owned by the caller (e.g. SourceManager's mapped files). Name may be
// empty when unknown.
struct SourceText {
  std::string_view name;
  std::string_view bytes;
};

// Returns the source text for a file id, or an empty SourceText when the
// file is unknown. Must never allocate.
using SourceFetch = SourceText (*)(u32 file_id, const void* ctx);

struct RenderOptions {
  bool color = false;
};

// Renders one diagnostic into out. Snippets are single-line
// (multi-line spans are clipped to their first line); secondary labels
// render as location lines. Only out itself may grow; inputs are only read.
void render(const Diagnostic& diag,
            fmt::memory_buffer& out,
            const RenderOptions& options = {},
            SourceFetch fetch = nullptr,
            const void* ctx = nullptr);

}  // namespace diag
