// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/sys`: the process's arguments.

// The number of arguments, the program name first.
pub unsafe intrinsic fn arg_count() -> i64;

// Argument `i` as a view, or an empty view when `i` is out of range.
// The bytes belong to the process.
pub unsafe intrinsic fn arg(i: i64) -> str;
