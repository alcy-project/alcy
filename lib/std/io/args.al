// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: the process arguments.

// The number of arguments, the program name first.
pub fn args_len() -> usize {
  ret unsafe { arg_count() } as usize
}
