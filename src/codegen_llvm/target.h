// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

// `Target` and the host triple are codegen's, not LLVM's: the pipeline
// asks what it builds for before a backend is chosen. This header keeps
// the names this module's sources already read.

#include "codegen/target.h"

namespace codegen_llvm {

using codegen::host_triple;
using codegen::Target;

}  // namespace codegen_llvm
