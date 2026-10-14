// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: an owned string.

// A growable sequence of bytes, owning the buffer it reads through.
// The shape mirrors `Vec<u8>`: `buf` is uninitialized storage, `len`
// counts the initialized prefix, and `cap` counts the reserved room.
// Pushing past capacity reallocates, so a `str` view into the buffer
// is a loan of the string like any `&T` into a `Vec<T>` is.
pub struct String {
  buf: &mut MaybeUninit<u8>,
  len: usize,
  cap: usize,
}

impl String {
  pub fn new() -> String {
    ret String { buf: unsafe { alloc::<u8>(0) }, len: 0, cap: 0 }
  }

  // Reserves room for `n` bytes without changing the length. A zero
  // `n` still yields a distinct, freeable block.
  pub fn with_capacity(n: usize) -> String {
    ret String { buf: unsafe { alloc::<u8>(n) }, len: 0, cap: n }
  }

  pub fn len(self: &Self) -> usize {
    ret self.len
  }

  // A new string with `A`-`Z` folded to lower case; other bytes copy.
  pub fn to_ascii_lowercase(self: &Self) -> String {
    mut out := String::with_capacity(self.len)
    s := self.as_str()
    mut i := 0 as usize
    while i < str_len(s) {
      b := str_byte(s, i)
      if b >= 0x41u8 && b <= 0x5au8 {
        out.push(b + 0x20u8)
      } else {
        out.push(b)
      }
      i = i + 1
    }
    ret out
  }

  // A new string with `a`-`z` folded to upper case; other bytes copy.
  pub fn to_ascii_uppercase(self: &Self) -> String {
    mut out := String::with_capacity(self.len)
    s := self.as_str()
    mut i := 0 as usize
    while i < str_len(s) {
      b := str_byte(s, i)
      if b >= 0x61u8 && b <= 0x7au8 {
        out.push(b - 0x20u8)
      } else {
        out.push(b)
      }
      i = i + 1
    }
    ret out
  }

  // A new string that repeats these bytes `n` times. A zero `n` is
  // empty.
  pub fn repeat(self: &Self, n: usize) -> String {
    mut out := String::with_capacity(self.len * n)
    mut k := 0 as usize
    while k < n {
      out.push_str(self.as_str())
      k = k + 1
    }
    ret out
  }

  // Keeps the first `n` bytes, dropping the rest. A byte has no
  // destructor, so the length is the only thing to change.
  pub fn truncate(mut self: &mut Self, n: usize) {
    if n >= self.len {
      ret
    }
    self.len = n
  }

  // Removes and answers the last byte, or `None` when empty.
  pub fn pop(mut self: &mut Self) -> Option<u8> {
    if self.len == 0 {
      ret Option::None
    }
    self.len = self.len - 1
    ret Option::Some(unsafe { *uninit_assume(elem_ptr(self.buf, self.len)) })
  }

  pub fn capacity(self: &Self) -> usize {
    ret self.cap
  }

  pub fn is_empty(self: &Self) -> bool {
    ret self.len == 0
  }

  // Appends `b`, growing the buffer when it is full.
  pub fn push(mut self: &mut Self, b: u8) {
    if self.len == self.cap {
      self.grow()
    }
    unsafe { uninit_write(elem_ptr(self.buf, self.len), b) }
    self.len = self.len + 1
  }

  // Appends every byte of `s`, growing once for the whole run.
  pub fn push_str(mut self: &mut Self, s: str) {
    n := str_len(s)
    self.reserve(n)
    mut i := 0 as usize
    while i < n {
      unsafe {
        uninit_write(elem_ptr(self.buf, self.len + i), str_byte(s, i))
      }
      i = i + 1
    }
    self.len = self.len + n
  }

  // Reserves room for `extra` more bytes.
  fn reserve(mut self: &mut Self, extra: usize) {
    needed := self.len + extra
    if needed < self.len {
      panic("capacity overflow")
    }
    if needed <= self.cap {
      ret
    }
    mut next := doubled_capacity(self.cap)
    if next < needed {
      next = needed
    }
    fresh := unsafe { alloc::<u8>(next) }
    unsafe {
      memcopy(elem_ptr(fresh, 0), elem_ref(self.buf, 0), self.len)
    }
    unsafe { dealloc(self.buf, self.cap) }
    self.buf = fresh
    self.cap = next
  }

  // Moves to a block twice the room, or to a first block of four. The
  // initialized prefix crosses as one byte copy.
  fn grow(mut self: &mut Self) {
    mut next := doubled_capacity(self.cap)
    if self.cap == 0 {
      next = 4
    }
    fresh := unsafe { alloc::<u8>(next) }
    unsafe {
      memcopy(elem_ptr(fresh, 0), elem_ref(self.buf, 0), self.len)
    }
    unsafe { dealloc(self.buf, self.cap) }
    self.buf = fresh
    self.cap = next
  }

  pub fn as_str(self: &Self) -> str {
    ret unsafe { str_from_parts(uninit_ref(elem_ref(self.buf, 0)), self.len) }
  }

  pub fn bytes(self: &Self) -> StrIter {
    ret self.as_str().bytes()
  }

  pub fn as_bytes(self: &Self) -> &[u8] {
    ret unsafe { slice_from_parts(uninit_ref(elem_ref(self.buf, 0)), self.len) }
  }

  // The byte-level predicates, so a `String` reads like the `str`
  // view it hands out.
  pub fn starts_with(self: &Self, prefix: str) -> bool {
    ret self.as_str().starts_with(prefix)
  }

  pub fn ends_with(self: &Self, suffix: str) -> bool {
    ret self.as_str().ends_with(suffix)
  }

  pub fn contains(self: &Self, needle: str) -> bool {
    ret self.as_str().contains(needle)
  }

  pub fn strip_prefix(self: &Self, prefix: str) -> Option<str> {
    ret self.as_str().strip_prefix(prefix)
  }

  pub fn trim_start_matches(self: &Self, pat: str) -> str {
    ret self.as_str().trim_start_matches(pat)
  }

  pub fn trim_end_matches(self: &Self, pat: str) -> str {
    ret self.as_str().trim_end_matches(pat)
  }

  pub fn count(self: &Self, needle: str) -> usize {
    ret self.as_str().count(needle)
  }

  pub fn lines(self: &Self) -> LinesIter {
    ret self.as_str().lines()
  }

  pub fn split_at(self: &Self, at: usize) -> (str, str) {
    ret self.as_str().split_at(at)
  }

  pub fn is_ascii(self: &Self) -> bool {
    ret self.as_str().is_ascii()
  }

  pub fn parse_i64(self: &Self) -> Option<i64> {
    ret self.as_str().parse_i64()
  }

  pub fn split_whitespace(self: &Self) -> SplitWhitespace {
    ret self.as_str().split_whitespace()
  }

  pub fn strip_suffix(self: &Self, suffix: str) -> Option<str> {
    ret self.as_str().strip_suffix(suffix)
  }

  pub fn find(self: &Self, needle: str) -> Option<usize> {
    ret self.as_str().find(needle)
  }

  // The trims answer a `str` view, so they borrow the buffer rather
  // than allocate.
  pub fn trim(self: &Self) -> str {
    ret self.as_str().trim()
  }

  pub fn trim_start(self: &Self) -> str {
    ret self.as_str().trim_start()
  }

  pub fn trim_end(self: &Self) -> str {
    ret self.as_str().trim_end()
  }

  pub fn split(self: &Self, delim: str) -> SplitIter {
    ret self.as_str().split(delim)
  }

  pub fn split_once(self: &Self, delim: str) -> Option<(str, str)> {
    ret self.as_str().split_once(delim)
  }

  pub fn eq_ignore_ascii_case(self: &Self, other: str) -> bool {
    ret self.as_str().eq_ignore_ascii_case(other)
  }

  pub fn rfind(self: &Self, needle: str) -> Option<usize> {
    ret self.as_str().rfind(needle)
  }

  // Forgets every byte, keeping the room already reserved.
  pub fn clear(mut self: &mut Self) {
    self.len = 0
  }

  pub fn drop(self: String) {
    unsafe { dealloc(self.buf, self.cap) }
  }
}

// `s[i]` reads a byte and `s[i] = b` writes one, panicking past the
// end the way array indexing does.
impl Index<usize, u8> for String {
  fn index(self: &Self, i: usize) -> &u8 {
    if i >= self.len {
      panic("String index out of bounds")
    }
    ret uninit_ref(unsafe { elem_ref(self.buf, i) })
  }
}

impl IndexMut<usize, u8> for String {
  fn index_mut(mut self: &mut Self, i: usize) -> &mut u8 {
    if i >= self.len {
      panic("String index out of bounds")
    }
    ret unsafe { uninit_assume(elem_ptr(self.buf, i)) }
  }
}

impl PartialEq for String {
  fn eq(self: &Self, other: &Self) -> bool {
    if self.len() != other.len() {
      ret false
    }
    mut i := 0 as usize
    while i < self.len() {
      if self[i] != other[i] {
        ret false
      }
      i = i + 1
    }
    ret true
  }
}

// Byte equality is reflexive, so `String` is `Eq`.
impl Eq for String {
}
