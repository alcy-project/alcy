// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "diag/diagnostic.h"
#include "fmt/format.h"
#include "i18n/language.h"
#include "source/source.h"

namespace diag {

// Zero-copy source lookup for the renderer: both views borrow from storage
// owned by the caller (e.g. SourceManager's mapped files).
struct SourceText {
  std::string_view name;
  std::string_view bytes;
};

// Returns the source text for a file id, or std::nullopt when the id does
// not name a known file - distinct from a known file whose bytes are
// empty. Must never allocate.
using SourceFetch = std::optional<SourceText> (*)(source::FileId file_id,
                                                  const void* ctx);

// Presentation choices supplied by the output layer. The renderer does not
// inspect the terminal or environment.
struct RenderOptions {
  bool color = false;
  // Language the chrome is written in: the severity word, the note that
  // labels a secondary span, and the stand-in for a file the manager
  // cannot name. The message itself was composed in this language when it
  // was emitted, so a bag and these options disagree only if a caller
  // renders one bag under two languages.
  i18n::Language language = i18n::Language::EnUs;
};

// Renders one diagnostic into out. Snippets are single-line
// (multi-line spans are clipped to their first line); secondary labels
// render as location lines. Only out itself may grow; inputs are only read.
void render(const Diagnostic& diag,
            fmt::memory_buffer& out,
            const RenderOptions& options = {},
            SourceFetch fetch = nullptr,
            const void* ctx = nullptr);

// `render` for a caller that wants the text rather than a buffer, and has
// no source to quote: the marker and the message, and nothing else. A
// message from outside a check area - a rejected command line, a flag
// combination the config check turned down - is exactly that, and a
// buffer it has to copy out of is a step with no other purpose.
std::string render(const Diagnostic& diag, const RenderOptions& options = {});

}  // namespace diag
