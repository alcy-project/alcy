// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `fmt` package.
//
// This file is the package's root module. Its `pub` items are the
// package's implicit surface.
//
// The compiler recognises `write` and `format` by this package rather
// than by their names; see docs/adr/0016. Both are expanded at compile
// time and neither declared body executes.

pub struct WriteOutcome { written: usize, total: usize }

pub fn write(comp fmt: str, buf: &mut [u8; 0], args: ()) -> WriteOutcome {
  panic("fmt::write must expand")
}

pub fn format(comp fmt: str, args: ()) -> String {
  mut out := String::new()
  result := write(fmt, &mut out.buf, args)
  if result.total != result.written {
    panic("format output truncated")
  }
  out.len = result.written
  ret out
}
