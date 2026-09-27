// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Toolchain standard library, `alcy/std` suite, `alloc` package.
//
// This file is the package's root module. Its `pub` items are the
// package's implicit surface.
//
// The heap and the values that live on it. `Vec` and `String` are here
// rather than in core because they are heap-backed, and leaving them in
// core while the primitives are here would make the two packages
// depend on each other; see docs/adr/0016.

pub intrinsic fn alloc<T>(count: usize) -> &mut MaybeUninit<T>;

pub intrinsic fn dealloc<T>(ptr: &mut MaybeUninit<T>, count: usize);

// Typed element offset. The result points at element `index` of the
// buffer `ptr` addresses, and is in bounds exactly when the caller keeps
// `index` within the buffer's length. Element type `T` is fixed by the
// pointee of `ptr`, so the call admits one instantiation.
pub intrinsic fn elem_ptr<T>(ptr: &mut MaybeUninit<T>, index: usize) -> &mut MaybeUninit<T>;

// Shared counterpart of `elem_ptr`, so a buffer is readable through a
// shared borrow of the value that owns it. `&mut T` coerces to `&T`, so
// the argument may name the buffer either way.
pub intrinsic fn elem_ref<T>(ptr: &MaybeUninit<T>, index: usize) -> &MaybeUninit<T>;

// Writes `value` into an uninitialized slot. Moving the value in leaves
// the slot initialized, so a later `uninit_assume` on it is sound.
pub intrinsic fn uninit_write<T>(slot: &mut MaybeUninit<T>, value: T);

// Releases a slot as a mutable reference to its value. Reading through
// the result before anything was written yields whatever the allocator
// returned.
pub intrinsic fn uninit_assume<T>(slot: &mut MaybeUninit<T>) -> &mut T;

// Shared counterpart of `uninit_assume`, with the same caveat: reading
// before anything was written yields whatever was there.
pub intrinsic fn uninit_ref<T>(slot: &MaybeUninit<T>) -> &T;

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
