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
    ret String { buf: alloc::<u8>(0), len: 0, cap: 0 }
  }

  // Reserves room for `n` bytes without changing the length. A zero
  // `n` still yields a distinct, freeable block.
  pub fn with_capacity(n: usize) -> String {
    ret String { buf: alloc::<u8>(n), len: 0, cap: n }
  }

  pub fn len(self: &Self) -> usize {
    ret self.len
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
    uninit_write(elem_ptr(self.buf, self.len), b)
    self.len = self.len + 1
  }

  // Appends every byte of `s`, growing as needed.
  pub fn push_str(mut self: &mut Self, s: str) {
    mut i := 0 as usize
    while i < str_len(s) {
      self.push(str_byte(s, i))
      i = i + 1
    }
  }

  // Moves to a block twice the room, or to a first block of four.
  fn grow(mut self: &mut Self) {
    mut next := self.cap * 2
    if self.cap == 0 {
      next = 4
    }
    fresh := alloc::<u8>(next)
    mut i := 0 as usize
    while i < self.len {
      uninit_write(elem_ptr(fresh, i), *uninit_assume(elem_ptr(self.buf, i)))
      i = i + 1
    }
    dealloc(self.buf, self.cap)
    self.buf = fresh
    self.cap = next
  }

  pub fn as_str(self: &Self) -> str {
    ret str_from_parts(uninit_ref(elem_ref(self.buf, 0)), self.len)
  }

  pub fn as_bytes(self: &Self) -> &[u8] {
    ret slice_from_parts(uninit_ref(elem_ref(self.buf, 0)), self.len)
  }

  // Forgets every byte, keeping the room already reserved.
  pub fn clear(mut self: &mut Self) {
    self.len = 0
  }

  pub fn drop(self: String) {
    dealloc(self.buf, self.cap)
  }
}
