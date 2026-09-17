// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

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
  // Numeric code rendered as E<code>/W<code>/N<code>. Ranges are partitioned
  // by producer; 1000-1999 is reserved for IR verification.
  u32 code = 0;
  std::string_view message;
  bool has_primary_span = false;
  Span primary_span;
  // Arena-owned array, possibly empty (labels == nullptr when empty).
  const Label* labels = nullptr;
  u32 label_count = 0;
};

}  // namespace diag
