// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/alloc`: the typed heap primitives.

// The capacity after one doubling step, panicking when the result
// would not fit in a `usize`. A wrapped capacity would name a far
// smaller block than the container records, so growth refuses the
// overflow before the allocator sees it.
pub fn doubled_capacity(cap: usize) -> usize {
  // The largest `usize`, through the wrapping form so a debug build
  // does not trip the checked subtraction.
  if cap > wrapping_sub(0 as usize, 1 as usize) / 2 {
    panic("capacity overflow")
  }
  ret cap * 2
}

// Heap allocation. `alloc<T>(count)` reserves room for `count`
// elements of `T`, using `T`'s own size and alignment. The returned
// reference uniquely owns the block and must be released with
// `dealloc<T>(ptr, count)`. `T` is fixed by the call's turbofish, so a
// call admits one instantiation.
pub unsafe intrinsic fn alloc<T>(count: usize) -> &mut MaybeUninit<T>;

pub unsafe intrinsic fn dealloc<T>(ptr: &mut MaybeUninit<T>, count: usize);

// Typed element offset. The result points at element `index` of the
// buffer `ptr` addresses, and is in bounds exactly when the caller keeps
// `index` within the buffer's length. Element type `T` is fixed by the
// pointee of `ptr`, so the call admits one instantiation.
pub unsafe intrinsic fn elem_ptr<T>(ptr: &mut MaybeUninit<T>, index: usize) -> &mut MaybeUninit<T>;

// Shared counterpart of `elem_ptr`, so a buffer is readable through a
// shared borrow of the value that owns it. `&mut T` coerces to `&T`, so
// the argument may name the buffer either way.
pub unsafe intrinsic fn elem_ref<T>(ptr: &MaybeUninit<T>, index: usize) -> &MaybeUninit<T>;

// Writes `value` into an uninitialized slot. Moving the value in leaves
// the slot initialized, so a later `uninit_assume` on it is sound.
pub intrinsic fn uninit_write<T>(slot: &mut MaybeUninit<T>, value: T);

// Releases a slot as a mutable reference to its value. Reading through
// the result before anything was written yields whatever the allocator
// returned.
pub unsafe intrinsic fn uninit_assume<T>(slot: &mut MaybeUninit<T>) -> &mut T;

// Shared counterpart of `uninit_assume`, with the same caveat: reading
// before anything was written yields whatever was there.
pub intrinsic fn uninit_ref<T>(slot: &MaybeUninit<T>) -> &T;

#[when(testing)] {
  #[test] fn doubled_capacity_takes_a_nonzero_capacity_to_twice_itself() {
    assert(doubled_capacity(1) == 2, "one element's room doubles to two")
    assert(doubled_capacity(4) == 8, "four elements' room doubles to eight")
  }

  #[test] fn doubled_capacity_of_zero_is_zero() {
    assert(doubled_capacity(0) == 0, "the doubling keeps an empty block empty")
  }

  #[test] fn a_written_slot_reads_back() {
    mut slot := unsafe { alloc::<i32>(1) }
    uninit_write(slot, 7)
    assert(*unsafe { uninit_assume(slot) } == 7, "the slot holds the value written into it")
    unsafe { dealloc(slot, 1) }
  }

  #[test] fn every_slot_of_a_block_holds_its_own_value() {
    mut block := unsafe { alloc::<i32>(4) }
    mut i := 0 as usize
    while i < 4 {
      uninit_write(unsafe { elem_ptr(block, i) }, (i as i32) * 10)
      i = i + 1
    }
    assert(*unsafe { uninit_ref(elem_ref(block, 0)) } == 0, "the first slot keeps the first value")
    assert(*unsafe { uninit_ref(elem_ref(block, 3)) } == 30, "the last slot keeps the last value")
    unsafe { dealloc(block, 4) }
  }

  #[test] fn a_block_of_no_elements_is_still_freeable() {
    empty := unsafe { alloc::<i32>(0) }
    unsafe { dealloc(empty, 0) }
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
