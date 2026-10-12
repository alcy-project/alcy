// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// `alcy/std/core`: borrowed slices over elements.

// A slice is a view into a run of `T`: a pointer plus a length, with
// no ownership. Slices only occur behind a reference (`&[T]`), the
// way `str` is a view into bytes; `str` stays its own type rather
// than spelling as `&[u8]`.
pub intrinsic fn slice_len<T>(s: &[T]) -> usize;

pub unsafe intrinsic fn slice_from_parts<T>(ptr: &T, len: usize) -> &[T];

pub unsafe intrinsic fn slice_from_parts_mut<T>(ptr: &mut T, len: usize) -> &mut [T];

// The element at `i`; in bounds exactly when `i < slice_len(s)`.
pub unsafe intrinsic fn slice_at<T>(s: &[T], i: usize) -> &T;

impl<T> [T] {
  // The element count.
  pub fn len(self: &Self) -> usize {
    ret slice_len(self)
  }

  pub fn is_empty(self: &Self) -> bool {
    ret slice_len(self) == 0
  }

  // A shared cursor over the elements; the slice's loan is what keeps
  // them valid.
  pub fn iter(self: &Self) -> SliceIter<T> {
    ret SliceIter { s: self, pos: 0 }
  }
}

// A shared cursor over a slice's elements.
pub struct SliceIter<T> {
  s: &[T],
  pos: usize,
}

impl<T> SliceIter<T> {
  pub fn into_iter(self: SliceIter<T>) -> SliceIter<T> {
    ret self
  }
}

impl<T> Iterator<&T> for SliceIter<T> {
  fn next(mut self: &mut Self) -> Option<&T> {
    if self.pos >= slice_len(self.s) {
      ret Option::None
    }
    v := unsafe { slice_at(self.s, self.pos) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

impl<T, N: usize> [T; N] {
  // The element count; `N` is the array's own.
  pub fn len(self: &Self) -> usize {
    ret N
  }

  pub fn is_empty(self: &Self) -> bool {
    ret N == 0
  }

  // The array as a shared view.
  pub fn as_slice(self: &Self) -> &[T] {
    ret unsafe { slice_from_parts(&(*self)[0], N) }
  }

  pub fn iter(self: &Self) -> SliceIter<T> {
    ret self.as_slice().iter()
  }
}
