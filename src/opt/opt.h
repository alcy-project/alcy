// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/result.h"
#include "ir/storage.h"
#include "ir/verifier.h"

namespace opt {

// The release pipeline's IR passes, run after borrow checking and
// before any backend. LLVM would fold and prune most of this itself,
// but a direct backend has no middle end, so the work belongs to the
// pipeline where both backends see it. The result is rebuilt through
// the verifier, so a backend still receives proved IR.
//
// Today the passes are function-local: constant folding with
// propagation into every use, then the instructions folding left
// without a use. Anything larger (devirtualization, bounds-check
// elision, allocation pairing) needs cross-block or cross-function
// facts and lands as its own pass here.
base::Result<ir::VerifiedStorage, ir::VerificationError> optimize(
    ir::VerifiedStorage storage);

}  // namespace opt
