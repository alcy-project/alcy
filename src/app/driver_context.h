// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "fpag/str/string_interner.h"
#include "source/source.h"

namespace app {

// Diagnostic codes 3000-3099 are reserved for the driver.
inline constexpr u32 kDriverNoManifest = 3000;
inline constexpr u32 kDriverIoError = 3001;
inline constexpr u32 kDriverNotImplemented = 3002;
inline constexpr u32 kDriverNoTargets = 3003;
inline constexpr u32 kDriverLinkError = 3004;

struct DriverContext {
  mem::Arena arena;
  source::SourceManager sources;
  diag::DiagBag bag;
  // Long-lived string pool for lowering and codegen (function names,
  // string literals). Must outlive every phase that reads its ids.
  str::StringInterner strings;

  DriverContext();
};

// Renders every diagnostic in the bag through the logger.
void report(const diag::DiagBag& bag, const source::SourceManager& sources);

}  // namespace app
