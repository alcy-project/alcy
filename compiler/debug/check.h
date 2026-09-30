// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/debug/check.h"

// CHECK (both debug/release)
#define CHECK(expr) FPAG_CHECK(expr)
#define CHECK_MSG(expr, msg) FPAG_CHECK_MSG(expr, msg)

#define RAW_CHECK(expr) FPAG_RAW_CHECK(expr)
#define RAW_CHECK_MSG(expr, msg) FPAG_RAW_CHECK_MSG(expr, msg)

#define CHECK_EQ(lhs, rhs) FPAG_CHECK_EQ(lhs, rhs)
#define CHECK_NE(lhs, rhs) FPAG_CHECK_NE(lhs, rhs)
#define CHECK_LT(lhs, rhs) FPAG_CHECK_LT(lhs, rhs)
#define CHECK_LE(lhs, rhs) FPAG_CHECK_LE(lhs, rhs)
#define CHECK_GT(lhs, rhs) FPAG_CHECK_GT(lhs, rhs)
#define CHECK_GE(lhs, rhs) FPAG_CHECK_GE(lhs, rhs)

#define CHECK_EQ_MSG(lhs, rhs, msg) FPAG_CHECK_EQ_MSG(lhs, rhs, msg)
#define CHECK_NE_MSG(lhs, rhs, msg) FPAG_CHECK_NE_MSG(lhs, rhs, msg)
#define CHECK_LT_MSG(lhs, rhs, msg) FPAG_CHECK_LT_MSG(lhs, rhs, msg)
#define CHECK_LE_MSG(lhs, rhs, msg) FPAG_CHECK_LE_MSG(lhs, rhs, msg)
#define CHECK_GT_MSG(lhs, rhs, msg) FPAG_CHECK_GT_MSG(lhs, rhs, msg)
#define CHECK_GE_MSG(lhs, rhs, msg) FPAG_CHECK_GE_MSG(lhs, rhs, msg)
