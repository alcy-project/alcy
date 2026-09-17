// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <limits>

#include "fpag/base/numeric.h"

namespace diag {

// Opaque identifier of a source file. Assigned by SourceManager (a future
// module); all other code treats it as an opaque key.
using SourceId = u32;
constexpr SourceId kUnknownSource = std::numeric_limits<SourceId>::max();

// A half-open byte range [offset, offset + length) within one source file.
// Zero-copy: spans never own bytes, they only locate views into source
// storage owned elsewhere (e.g. memory-mapped files held by SourceManager).
struct Span {
  SourceId file = kUnknownSource;
  u32 offset = 0;
  u32 length = 0;
};

}  // namespace diag
