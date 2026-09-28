// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: an owned, growable sequence.

// A growable sequence of `T` values, owning the buffer it reads
// through.
//
// The buffer is uninitialized storage, so an element is readable only
// between being pushed and being popped or overwritten. `T` has to be
// movable by assignment, which is what lets a reallocation move an
// element from the old buffer to the new one.
//
// `at` takes `&Self` and hands out `&T`; the mutating accessors take
// `&mut Self`. A shared reborrow of `*self` reaches the buffer field,
// so a read-only view is a shared loan of the referent, not a copy of
// the pointer. See docs/adr/0012.
pub struct Vec<T> {
  buf: &mut MaybeUninit<T>,
  len: usize,
  cap: usize,
}

impl<T> Vec<T> {
  pub fn new() -> Vec<T> {
    ret Vec { buf: alloc::<T>(0), len: 0, cap: 0 }
  }

  // Reserves room for `n` elements without changing the length. A zero
  // `n` still yields a distinct, freeable block.
  pub fn with_capacity(n: usize) -> Vec<T> {
    ret Vec { buf: alloc::<T>(n), len: 0, cap: n }
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

  // Appends `value`, growing the buffer when it is full.
  pub fn push(mut self: &mut Self, value: T) {
    if self.len == self.cap {
      self.grow()
    }
    uninit_write(elem_ptr(self.buf, self.len), value)
    self.len = self.len + 1
  }

  // Moves to a block twice the room, or to a first block of four.
  // Elements are moved to the new buffer, so a `T` carrying a
  // destructor does not get one run for them; see docs/spec/deferred.md.
  fn grow(mut self: &mut Self) {
    mut next := self.cap * 2
    if self.cap == 0 {
      next = 4
    }
    fresh := alloc::<T>(next)
    mut i := 0 as usize
    while i < self.len {
      uninit_write(elem_ptr(fresh, i), *uninit_assume(elem_ptr(self.buf, i)))
      i = i + 1
    }
    dealloc(self.buf, self.cap)
    self.buf = fresh
    self.cap = next
  }

  // The element at `index`, or `None` when `index` is past the end. A
  // shared borrow of the vector, so it stays readable while the result
  // is live.
  pub fn at(self: &Self, index: usize) -> Option<&T> {
    if index >= self.len {
      ret Option::None
    }
    ret Option::Some(uninit_ref(elem_ref(self.buf, index)))
  }

  pub fn at_mut(mut self: &mut Self, index: usize) -> Option<&mut T> {
    if index >= self.len {
      ret Option::None
    }
    ret Option::Some(uninit_assume(elem_ptr(self.buf, index)))
  }

  // Removes and returns the last element, or `None` when empty.
  pub fn pop(mut self: &mut Self) -> Option<T> {
    if self.len == 0 {
      ret Option::None
    }
    self.len = self.len - 1
    ret Option::Some(*uninit_assume(elem_ptr(self.buf, self.len)))
  }

  // Forgets every element, keeping the room already reserved. A `T`
  // carrying a destructor is not ended.
  pub fn clear(mut self: &mut Self) {
    self.len = 0
  }

  pub fn drop(self: Vec<T>) {
    dealloc(self.buf, self.cap)
  }
}
