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

// The mutable element at `i`; in bounds exactly when `i < slice_len(s)`.
pub unsafe intrinsic fn slice_at_mut<T>(s: &mut [T], i: usize) -> &mut T;

impl<T> [T] {
  // The element count.
  pub fn len(self: &Self) -> usize {
    ret slice_len(self)
  }

  pub fn is_empty(self: &Self) -> bool {
    ret slice_len(self) == 0
  }

  // The first element, or `None` when empty.
  pub fn first(self: &Self) -> Option<&T> {
    if slice_len(self) == 0 {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at(self, 0) })
  }

  // The last element, or `None` when empty.
  pub fn last(self: &Self) -> Option<&T> {
    n := slice_len(self)
    if n == 0 {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at(self, n - 1) })
  }

  // A shared cursor over the elements; the slice's loan is what keeps
  // them valid.
  pub fn iter(self: &Self) -> SliceIter<T> {
    ret SliceIter { s: self, pos: 0 }
  }

  // An exclusive cursor over the elements.
  pub fn iter_mut(mut self: &mut Self) -> SliceIterMut<T> {
    ret SliceIterMut { s: self, pos: 0 }
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

  // The array as a mutable view.
  pub fn as_mut_slice(mut self: &mut Self) -> &mut [T] {
    ret unsafe { slice_from_parts_mut(&mut (*self)[0], N) }
  }

  pub fn iter(self: &Self) -> SliceIter<T> {
    ret self.as_slice().iter()
  }

  pub fn iter_mut(mut self: &mut Self) -> SliceIterMut<T> {
    ret SliceIterMut { s: self.as_mut_slice(), pos: 0 }
  }

  // Consumes the array; the cursor owns what is left.
  pub fn into_iter(self: [T; N]) -> ArrayIntoIter<T, N> {
    ret ArrayIntoIter { a: self, pos: 0 }
  }
}

// A consuming cursor over an array. Each element leaves by value; the
// cursor's destructor ends the ones still there, so a loop that stops
// early leaks nothing.
pub struct ArrayIntoIter<T, N: usize> {
  a: [T; N],
  pos: usize,
}

impl<T, N: usize> ArrayIntoIter<T, N> {
  pub fn into_iter(self: ArrayIntoIter<T, N>) -> ArrayIntoIter<T, N> {
    ret self
  }

  pub fn drop(mut self: ArrayIntoIter<T, N>) {
    mut i := self.pos
    while i < N {
      unsafe { drop_in_place(slice_at_mut(self.a.as_mut_slice(), i)) }
      i = i + 1
    }
  }
}

impl<T, N: usize> Iterator<T> for ArrayIntoIter<T, N> {
  fn next(mut self: &mut Self) -> Option<T> {
    if self.pos >= N {
      ret Option::None
    }
    at := unsafe { slice_at(self.a.as_slice(), self.pos) } as *T
    v := unsafe { read(at) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

// An exclusive cursor over a slice's elements.
pub struct SliceIterMut<T> {
  s: &mut [T],
  pos: usize,
}

impl<T> SliceIterMut<T> {
  pub fn into_iter(self: SliceIterMut<T>) -> SliceIterMut<T> {
    ret self
  }
}

impl<T> Iterator<&mut T> for SliceIterMut<T> {
  fn next(mut self: &mut Self) -> Option<&mut T> {
    if self.pos >= slice_len(self.s) {
      ret Option::None
    }
    v := unsafe { slice_at_mut(self.s, self.pos) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}
