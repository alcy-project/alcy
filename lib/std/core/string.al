// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: the `str` view over bytes.

pub intrinsic fn str_len(s: str) -> usize;

pub intrinsic fn str_byte(s: str, i: usize) -> u8;

pub intrinsic fn str_slice(s: str, start: usize, end: usize) -> str;

pub unsafe intrinsic fn str_from_parts(ptr: &u8, len: usize) -> str;

// An ASCII letter folded to lower case; other bytes pass through.
fn ascii_lower(b: u8) -> u8 {
  if b >= 0x41u8 && b <= 0x5au8 {
    ret b + 0x20u8
  }
  ret b
}

// The ASCII whitespace `trim` strips: space, tab, newline, vertical
// tab, form feed, and carriage return.
fn is_ascii_space(b: u8) -> bool {
  if b == 0x20u8 { ret true }
  if b == 0x09u8 { ret true }
  if b == 0x0au8 { ret true }
  if b == 0x0bu8 { ret true }
  if b == 0x0cu8 { ret true }
  if b == 0x0du8 { ret true }
  ret false
}

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

  // Whether the bytes begin with `prefix`. An empty prefix always
  // matches.
  pub fn starts_with(self: &Self, prefix: str) -> bool {
    n := str_len(prefix)
    if str_len(*self) < n {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if str_byte(*self, i) != str_byte(prefix, i) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // Whether the bytes end with `suffix`. An empty suffix always
  // matches.
  pub fn ends_with(self: &Self, suffix: str) -> bool {
    n := str_len(suffix)
    m := str_len(*self)
    if m < n {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if str_byte(*self, m - n + i) != str_byte(suffix, i) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // Whether `needle` appears anywhere in the bytes. An empty needle
  // always matches.
  pub fn contains(self: &Self, needle: str) -> bool {
    ret self.find(needle).is_some()
  }

  // The byte offset where `needle` first appears, or `None`. An empty
  // needle is at offset zero.
  pub fn find(self: &Self, needle: str) -> Option<usize> {
    n := str_len(needle)
    m := str_len(*self)
    if n == 0 {
      ret Option::Some(0 as usize)
    }
    if n > m {
      ret Option::None
    }
    mut i := 0 as usize
    while i <= m - n {
      mut j := 0 as usize
      while j < n && str_byte(*self, i + j) == str_byte(needle, j) {
        j = j + 1
      }
      if j == n {
        ret Option::Some(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // The view with leading ASCII whitespace removed.
  pub fn trim_start(self: &Self) -> str {
    n := str_len(*self)
    mut i := 0 as usize
    while i < n && is_ascii_space(str_byte(*self, i)) {
      i = i + 1
    }
    ret str_slice(*self, i, n)
  }

  // The view with trailing ASCII whitespace removed.
  pub fn trim_end(self: &Self) -> str {
    n := str_len(*self)
    mut end := n
    while end > 0 && is_ascii_space(str_byte(*self, end - 1)) {
      end = end - 1
    }
    ret str_slice(*self, 0, end)
  }

  // The view with both ends trimmed. Only whitespace outside the
  // bytes is removed, so the result is a sub-view of the original.
  pub fn trim(self: &Self) -> str {
    ret self.trim_start().trim_end()
  }

  // The byte offset where `needle` last appears, or `None`. An empty
  // needle is at the end.
  pub fn rfind(self: &Self, needle: str) -> Option<usize> {
    n := str_len(needle)
    m := str_len(*self)
    if n == 0 {
      ret Option::Some(m)
    }
    if n > m {
      ret Option::None
    }
    mut i := m - n
    mut found := false
    mut at := 0 as usize
    while !found {
      mut j := 0 as usize
      while j < n && str_byte(*self, i + j) == str_byte(needle, j) {
        j = j + 1
      }
      if j == n {
        found = true
        at = i
      } else {
        if i == 0 {
          ret Option::None
        }
        i = i - 1
      }
    }
    ret Option::Some(at)
  }

  // Whether the bytes match `other` ignoring ASCII case.
  pub fn eq_ignore_ascii_case(self: &Self, other: str) -> bool {
    n := str_len(*self)
    if n != str_len(other) {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if ascii_lower(str_byte(*self, i)) != ascii_lower(str_byte(other, i)) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // The pieces either side of the first `delim`, or `None` when it
  // does not appear. An empty delimiter splits at the start.
  pub fn split_once(self: &Self, delim: str) -> Option<(str, str)> {
    match self.find(delim) {
      Option::Some(i) => {
        after := i + str_len(delim)
        ret Option::Some(
            (str_slice(*self, 0, i), str_slice(*self, after, str_len(*self))))
      }
      Option::None => { ret Option::None }
    }
  }

  // A cursor over the pieces `delim` separates. An empty delimiter
  // yields the whole view once, so the cursor always advances. The
  // receiver is taken by value: a view is Copy, and a borrow of the
  // cursor's source would otherwise outlive the temporary that made
  // it.
  pub fn split(self: str, delim: str) -> SplitIter {
    ret SplitIter { rest: self, delim: delim, done: false }
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

// A cursor over the pieces a delimiter separates.
pub struct SplitIter {
  rest: str,
  delim: str,
  done: bool,
}

impl SplitIter {
  pub fn into_iter(self: SplitIter) -> SplitIter {
    ret self
  }
}

impl Iterator<str> for SplitIter {
  fn next(mut self: &mut Self) -> Option<str> {
    if self.done {
      ret Option::None
    }
    if str_len(self.delim) == 0 {
      self.done = true
      ret Option::Some(self.rest)
    }
    match self.rest.find(self.delim) {
      Option::Some(i) => {
        piece := str_slice(self.rest, 0, i)
        self.rest =
            str_slice(self.rest, i + str_len(self.delim), str_len(self.rest))
        ret Option::Some(piece)
      }
      Option::None => {
        self.done = true
        ret Option::Some(self.rest)
      }
    }
  }
}
