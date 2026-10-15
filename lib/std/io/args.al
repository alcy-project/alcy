// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/io`: the process arguments.

// The number of arguments, the program name first.
pub fn args_len() -> usize {
  ret unsafe { arg_count() } as usize
}

// Argument `i` copied into a new string, or `None` when the index is
// past the end. The program name is index zero.
pub fn arg_at(i: usize) -> Option<String> {
  if (i as i64) >= unsafe { arg_count() } {
    ret Option::None
  }
  view := unsafe { arg(i as i64) }
  mut s := String::with_capacity(str_len(view))
  s.push_str(view)
  ret Option::Some(s)
}

// The arguments, in order, the program name first. The bytes are
// copied out, so the result is this program's own.
pub fn args() -> Vec<String> {
  mut out := Vec::<String>::with_capacity(args_len())
  n := args_len()
  mut i := 0 as usize
  while i < n {
    out.push(arg_at(i).unwrap())
    i = i + 1
  }
  ret out
}
