// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: an owned string.

// A string over a fixed block for now, so a string longer than it
// panics rather than growing. Making it growable needs `fmt`'s
// compile-time expansion to allocate instead of writing into an inline
// array; see docs/spec/deferred.md.
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
