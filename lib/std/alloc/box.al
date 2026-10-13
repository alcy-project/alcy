// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: the owning pointer.

// A value on the heap, owned by this handle: the destructor ends the
// pointee with `drop_in_place` and releases the block, which is what
// lets a recursive type pass through the indirection. The handle is
// move-only, because a buffer behind an exclusive reference is not a
// value to copy.
//
// `into_inner` moves the pointee out and releases the block; `live`
// is what says so. The destructor reads it and skips a handle that
// has nothing left to end, so the block is not released twice.
pub struct Box<T> {
  cell: &mut MaybeUninit<T>,
  live: bool,
}

impl<T> Box<T> {
  // Allocates room for `value` and moves it in.
  pub fn new(value: T) -> Box<T> {
    cell := unsafe { alloc::<T>(1) }
    uninit_write(cell, value)
    ret Box { cell: cell, live: true }
  }

  // Borrows the pointee.
  pub fn get(self: &Self) -> &T {
    ret unsafe { uninit_ref(self.cell) }
  }

  pub fn get_mut(mut self: &mut Self) -> &mut T {
    ret unsafe { uninit_assume(self.cell) }
  }

  // Moves the pointee out and releases the block; the handle is spent.
  pub fn into_inner(mut self: Box<T>) -> T {
    value := unsafe { read(uninit_ref(self.cell) as *T) }
    unsafe { dealloc(self.cell, 1) }
    self.live = false
    ret value
  }

  pub fn drop(self: Box<T>) {
    if !self.live {
      ret
    }
    unsafe { drop_in_place::<T>(uninit_assume(self.cell)) }
    unsafe { dealloc(self.cell, 1) }
  }
}
