// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/fmt`: formatting into an owned string.

pub fn format(comp fmt: str, args: ()) -> String {
  mut out := String::new()
  result := write(fmt, &mut out.buf, args)
  if result.total != result.written {
    panic("format output truncated")
  }
  out.len = result.written
  ret out
}
