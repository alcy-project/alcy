// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <limits>

#include "fpag/base/numeric.h"
#include "source/source.h"

namespace diag {

// A half-open byte range [offset, offset + length) within one source file.
// Zero-copy: spans never own bytes, they only locate views into source
// storage owned elsewhere (e.g. memory-mapped files held by SourceManager).
struct Span {
  source::FileId file = source::kUnknownFile;
  u32 offset = 0;
  u32 length = 0;
};

}  // namespace diag
