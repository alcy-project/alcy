// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, core member.
//
// This file ships inside the compiler binary and is injected as a
// prelude module into every compilation, so its public items need no
// imports. It declares compiler-provided intrinsics only; everything
// else in core arrives as ordinary library code later.

pub intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);

pub intrinsic fn panic(msg: str) -> !;

intrinsic fn sys_write(fd: i32, buf: str);

pub fn print(msg: str) {
  sys_write(1, msg)
}

pub fn println(msg: str) {
  sys_write(1, msg)
  sys_write(1, "\n")
}

pub intrinsic fn str_len(s: str) -> usize;

pub intrinsic fn str_byte(s: str, i: usize) -> u8;

pub intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;

pub struct WriteOutcome { written: usize, total: usize }

intrinsic fn str_from_parts(ptr: &u8, len: usize) -> str;

pub struct String { buf: [u8; 256], len: usize }

impl String {
  pub fn new() -> String {
    ret String { buf: [0u8; 256], len: 0 }
  }

  pub fn len(self: &Self) -> usize {
    ret self.len
  }

  pub fn push(mut self: &mut Self, b: u8) {
    if self.len >= 256 {
      panic("String is full")
    }
    self.buf[self.len] = b
    self.len = self.len + 1
  }

  pub fn as_str(self: &Self) -> str {
    ret str_from_parts(&self.buf[0], self.len)
  }
}

// Formats `args` into an owned string; see docs/spec/fmt.md.
// Outputs longer than the internal buffer abort rather than
// truncating silently.
pub fn format(comp fmt: str, args: ()) -> String {
  mut out := String::new()
  result := write(fmt, &mut out.buf, args)
  if result.total != result.written {
    panic("format output truncated")
  }
  out.len = result.written
  ret out
}

// Formats `args` into `buf` by compile-time expansion; see
// docs/spec/fmt.md. Calls check and expand through compiler support
// (like the print intrinsics); the body never executes, and reaching
// it aborts. The `[u8; 0]` and `()` parameter types are placeholders:
// calls accept any byte-array size and any tuple arity through custom
// checking, since the language cannot name them yet.
pub fn write(comp fmt: str, buf: &mut [u8; 0], args: ()) -> WriteOutcome {
  panic("fmt::write must expand")
}
