// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/sort`: an unstable sort over borrowed slices.
//
// Heap sort runs in place with no auxiliary storage. Equal elements
// have no guaranteed order; callers that need one use `stable`.

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

// Moves the element at `root` down until `less` holds between each
// parent and its children. `end` bounds the heap inside the slice.
fn sift_down<T>(s: &mut [T], root: usize, end: usize, less: (&T, &T) -> bool) {
  mut parent := root
  loop {
    left := parent * 2 + 1
    if left >= end {
      break
    }
    mut child := left
    right := left + 1
    if right < end {
      c := unsafe { slice_at(s, right) }
      l := unsafe { slice_at(s, left) }
      if less(l, c) {
        child = right
      }
    }
    p := unsafe { slice_at(s, parent) }
    ch := unsafe { slice_at(s, child) }
    if !less(p, ch) {
      break
    }
    swap(s, parent, child)
    parent = child
  }
}

// Sorts a slice in place. Equal elements have no guaranteed order.
pub fn sort_by<T>(s: &mut [T], less: (&T, &T) -> bool) {
  n := slice_len(s)
  // Build a max-heap under `less`: a parent must not be less than its
  // child. The second half of the slice starts as leaves already.
  mut start := n / 2
  while start > 0 {
    start = start - 1
    sift_down(s, start, n, less)
  }
  // Move each root to the end, shrinking the heap by one.
  mut end := n
  while end > 1 {
    end = end - 1
    swap(s, 0, end)
    sift_down(s, 0, end, less)
  }
}

// Sorts a slice by `Ord`. A type with only `PartialOrd` uses
// `sort_by`, the same as the stable sort. The comparison is inlined
// rather than wrapped in a closure: a spec dispatch needs its
// implementation record at lowering, which a stored closure does not
// carry.
pub fn sort<T: Ord>(s: &mut [T]) {
  n := slice_len(s)
  mut start := n / 2
  while start > 0 {
    start = start - 1
    sift_down_ord(s, start, n)
  }
  mut end := n
  while end > 1 {
    end = end - 1
    swap(s, 0, end)
    sift_down_ord(s, 0, end)
  }
}

fn sift_down_ord<T: Ord>(s: &mut [T], root: usize, end: usize) {
  mut parent := root
  loop {
    left := parent * 2 + 1
    if left >= end {
      break
    }
    mut child := left
    right := left + 1
    if right < end {
      c := unsafe { slice_at(s, right) }
      l := unsafe { slice_at(s, left) }
      if l.cmp(c).is_lt() {
        child = right
      }
    }
    p := unsafe { slice_at(s, parent) }
    ch := unsafe { slice_at(s, child) }
    if !p.cmp(ch).is_lt() {
      break
    }
    swap(s, parent, child)
    parent = child
  }
}

#[when(testing)] {
  #[test] fn unstable_sort_orders_the_elements() {
    mut a := [3i32, 1, 2, 5, 4]
    sort(a.as_mut_slice())
    assert(a[0] == 1 && a[1] == 2 && a[2] == 3 && a[3] == 4 && a[4] == 5, "the ascending order")
  }

  #[test] fn unstable_sort_by_takes_the_predicate() {
    mut a := [3i32, 1, 2, 5, 4]
    sort_by(a.as_mut_slice(), (x: &i32, y: &i32) -> *x > *y)
    assert(a[0] == 5 && a[1] == 4 && a[2] == 3 && a[3] == 2 && a[4] == 1, "the descending order")
  }

  fn assert(ok: bool, what: str) {
    if !ok {
      panic(what)
    }
  }
}
