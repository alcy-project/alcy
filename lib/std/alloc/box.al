// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: the owning pointer.

// A value on the heap, owned by this handle: the destructor ends the
// pointee with `drop_in_place` and releases the block, which is what
// lets a recursive type pass through the indirection. The handle is
// move-only, because a buffer behind an exclusive reference is not a
// value to copy.
//
// The pointee is reached through `get`/`get_mut`: an owning deref is a
// language slice of its own (see `docs/backlog.md`), and so is
// releasing the block while moving the pointee out (`into_inner`),
// which needs a spent handle the destructor can recognize.
pub struct Box<T> {
  cell: &mut MaybeUninit<T>,
}

impl<T> Box<T> {
  // Allocates room for `value` and moves it in.
  pub fn new(value: T) -> Box<T> {
    cell := unsafe { alloc::<T>(1) }
    uninit_write(cell, value)
    ret Box { cell: cell }
  }

  // Borrows the pointee.
  pub fn get(self: &Self) -> &T {
    ret unsafe { uninit_ref(self.cell) }
  }

  pub fn get_mut(mut self: &mut Self) -> &mut T {
    ret unsafe { uninit_assume(self.cell) }
  }

  pub fn drop(self: Box<T>) {
    unsafe { drop_in_place::<T>(uninit_assume(self.cell)) }
    unsafe { dealloc(self.cell, 1) }
  }
}
