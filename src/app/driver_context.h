// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace app {

// Diagnostic codes 3000-3099 are reserved for the driver.
inline constexpr u32 kDriverNoManifest = 3000;
inline constexpr u32 kDriverIoError = 3001;
inline constexpr u32 kDriverNotImplemented = 3002;
inline constexpr u32 kDriverNoTargets = 3003;

struct DriverContext {
  mem::Arena arena;
  source::SourceManager sources;
  diag::DiagBag bag;

  DriverContext();
};

// Renders every diagnostic in the bag through the logger.
void report(const diag::DiagBag& bag, const source::SourceManager& sources);

}  // namespace app
