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

#[when(testing)] {
  #[test] fn a_new_box_holds_the_value_it_was_built_from() {
    b := Box::<i32>::new(42)
    assert(*b.get() == 42, "the box reads back the value it was built from")
  }

  #[test] fn get_mut_writes_through_to_the_value() {
    mut b := Box::<i32>::new(1)
    slot := b.get_mut()
    *slot = 2
    assert(*b.get() == 2, "the write lands in the box's own value")
  }

  #[test] fn into_inner_reads_the_value_out() {
    b := Box::<i32>::new(7)
    assert(b.into_inner() == 7, "the value comes back out of the handle")
  }

  #[test] fn a_box_carries_a_value_that_is_not_a_scalar() {
    mut b := Box::<Vec<i32>>::new(Vec::<i32>::new())
    held := b.get_mut()
    held.push(5)
    inner := b.into_inner()
    assert(inner.len() == 1, "the boxed vec keeps what was pushed into it")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
