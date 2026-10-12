// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the `str` view over bytes.

pub intrinsic fn str_len(s: str) -> usize;

pub intrinsic fn str_byte(s: str, i: usize) -> u8;

pub intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;

pub unsafe intrinsic fn str_from_parts(ptr: &u8, len: usize) -> str;

impl str {
  // The byte length.
  pub fn len(self: &Self) -> usize {
    ret str_len(*self)
  }

  pub fn is_empty(self: &Self) -> bool {
    ret str_len(*self) == 0
  }

  // The byte at `at`; in bounds exactly when `at < self.len()`.
  pub fn byte(self: &Self, at: usize) -> u8 {
    ret str_byte(*self, at)
  }

  // Byte iteration: `for b in s` reads each byte in order.
  pub fn into_iter(self: str) -> StrIter {
    ret StrIter { s: self, pos: 0 }
  }
}

// A consuming cursor over a string's bytes.
pub struct StrIter {
  s: str,
  pos: usize,
}

impl StrIter {
  pub fn into_iter(self: StrIter) -> StrIter {
    ret self
  }
}

impl Iterator<u8> for StrIter {
  fn next(mut self: &mut Self) -> Option<u8> {
    if self.pos >= str_len(self.s) {
      ret Option::None
    }
    b := str_byte(self.s, self.pos)
    self.pos = self.pos + 1
    ret Option::Some(b)
  }
}
