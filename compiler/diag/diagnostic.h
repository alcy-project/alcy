// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string_view>

#include "diag/span.h"
#include "fpag/base/numeric.h"

namespace diag {

enum class Severity : u8 {
  Note,
  Warning,
  Error,
};

// A secondary labeled span attached to a Diagnostic (e.g. "defined here",
// "expected because of this call"). Message may be empty.
struct Label {
  Span span;
  std::string_view message;
};

// A single diagnostic: severity, numeric code, message, one
// primary span, and any number of secondary labels. All strings are views;
// message bytes live in arena storage owned by DiagBag (or are static), and
// spans locate views into source storage owned by SourceManager. Copying a
// Diagnostic is always cheap and never allocates.
struct Diagnostic {
  Severity severity = Severity::Error;
  // Numeric code rendered as E<code>/W<code>/N<code>. Ranges are
  // partitioned by producer; see compiler/docs/diagnostics.md. Empty for
  // a message from outside a check area, which renders as `error: `
  // rather than inventing a number nobody allocated.
  std::optional<u32> code;
  std::string_view message;
  bool has_primary_span = false;
  Span primary_span;
  // Arena-owned array, possibly empty (labels == nullptr when empty).
  const Label* labels = nullptr;
  u32 label_count = 0;
};

// A message with no span and no code: a complaint about a command line, a
// configuration, or a process, where there is no source to point at and no
// check area allocated a number. The severity is then all a reader has to
// go on, which is why the marker says `error: ` rather than inventing a
// code. Most of what a command line reports is one of these.
constexpr Diagnostic message(Severity severity, std::string_view text) {
  return Diagnostic{
      .severity = severity,
      .code = std::nullopt,
      .message = text,
      .primary_span = {},
  };
}

}  // namespace diag
