// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/debug/check.h"

// DCHECK (debug only)
#define DCHECK(expr) FPAG_DCHECK(expr)
#define DCHECK_MSG(expr, msg) FPAG_DCHECK_MSG(expr, msg)

#define RAW_DCHECK(expr) FPAG_RAW_DCHECK(expr)
#define RAW_DCHECK_MSG(expr, msg) FPAG_RAW_DCHECK_MSG(expr, msg)

#define DCHECK_EQ(lhs, rhs) FPAG_DCHECK_EQ(lhs, rhs)
#define DCHECK_NE(lhs, rhs) FPAG_DCHECK_NE(lhs, rhs)
#define DCHECK_LT(lhs, rhs) FPAG_DCHECK_LT(lhs, rhs)
#define DCHECK_LE(lhs, rhs) FPAG_DCHECK_LE(lhs, rhs)
#define DCHECK_GT(lhs, rhs) FPAG_DCHECK_GT(lhs, rhs)
#define DCHECK_GE(lhs, rhs) FPAG_DCHECK_GE(lhs, rhs)

#define DCHECK_EQ_MSG(lhs, rhs, msg) FPAG_DCHECK_EQ_MSG(lhs, rhs, msg)
#define DCHECK_NE_MSG(lhs, rhs, msg) FPAG_DCHECK_NE_MSG(lhs, rhs, msg)
#define DCHECK_LT_MSG(lhs, rhs, msg) FPAG_DCHECK_LT_MSG(lhs, rhs, msg)
#define DCHECK_LE_MSG(lhs, rhs, msg) FPAG_DCHECK_LE_MSG(lhs, rhs, msg)
#define DCHECK_GT_MSG(lhs, rhs, msg) FPAG_DCHECK_GT_MSG(lhs, rhs, msg)
#define DCHECK_GE_MSG(lhs, rhs, msg) FPAG_DCHECK_GE_MSG(lhs, rhs, msg)
