// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/sort`: sorting over borrowed slices.
//
// The sort is stable: equal elements keep the order they had. It works
// on any writable slice and takes the order as a predicate, so it
// needs no container and no `Ord` bound.

use core::cmp::Ordering;
use core::mem::read;
use core::slice::slice_at_mut;

// Exchanges two elements of a slice. The raw pointer reads are moved
// through, so a `T` with a destructor is not copied here.
fn swap<T>(s: &mut [T], i: usize, j: usize) {
  mut a := unsafe { slice_at_mut(s, i) as *mut T }
  mut b := unsafe { slice_at_mut(s, j) as *mut T }
  va := unsafe { read(a as *T) }
  vb := unsafe { read(b as *T) }
  unsafe {
    *a = vb
    *b = va
  }
}

// Sorts a slice in place with `less` deciding the order. Equal
// elements keep their relative order.
pub fn sort_by<T>(s: &mut [T], less: (&T, &T) -> bool) {
  n := slice_len(s)
  mut i := 1 as usize
  while i < n {
    mut j := i
    while j > 0 {
      left := unsafe { slice_at(s, j - 1) }
      right := unsafe { slice_at(s, j) }
      if !less(right, left) {
        break
      }
      swap(s, j - 1, j)
      j = j - 1
    }
    i = i + 1
  }
}

// Sorts a slice by `Ord`. The total order has to exist for the sort to
// mean one order; a type with only `PartialOrd` uses `sort_by`.
pub fn sort<T: Ord>(s: &mut [T]) {
  n := slice_len(s)
  mut i := 1 as usize
  while i < n {
    mut j := i
    while j > 0 {
      left := unsafe { slice_at(s, j - 1) }
      right := unsafe { slice_at(s, j) }
      if !right.cmp(left).is_lt() {
        break
      }
      swap(s, j - 1, j)
      j = j - 1
    }
    i = i + 1
  }
}

#[when(testing)] {
  #[test] fn sort_orders_the_elements() {
    mut a := [3i32, 1, 2]
    sort(a.as_mut_slice())
    assert(a[0] == 1 && a[1] == 2 && a[2] == 3, "the ascending order")
  }

  #[test] fn sort_by_takes_the_predicate() {
    mut a := [3i32, 1, 2]
    sort_by(a.as_mut_slice(), (x: &i32, y: &i32) -> *x > *y)
    assert(a[0] == 3 && a[1] == 2 && a[2] == 1, "the descending order")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
