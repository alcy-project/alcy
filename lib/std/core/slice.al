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

impl<T>[T] {
  // The element count.
  pub fn len(self: &Self) -> usize {
    ret slice_len(self)
  }

  pub fn is_empty(self: &Self) -> bool {
    ret slice_len(self) == 0
  }

  // The element at `index`, or `None` when past the end.
  pub fn at(self: &Self, index: usize) -> Option<&T> {
    if index >= slice_len(self) {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at(self, index) })
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

  // The index of the first element the predicate accepts, or `None`.
  pub fn position(self: &Self, pred: (&T) -> bool) -> Option<usize> {
    mut i := 0 as usize
    while i < slice_len(self) {
      if pred(self.at(i).or_panic_with_message("the index is within the slice")) {
        ret Option::Some(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // The first element the predicate accepts, or `None`.
  pub fn find(self: &Self, pred: (&T) -> bool) -> Option<&T> {
    mut i := 0 as usize
    while i < slice_len(self) {
      if pred(self.at(i).or_panic_with_message("the index is within the slice")) {
        ret self.at(i)
      }
      i = i + 1
    }
    ret Option::None
  }

  // The last index the predicate accepts, or `None`.
  pub fn rposition(self: &Self, pred: (&T) -> bool) -> Option<usize> {
    mut i := slice_len(self)
    while i > 0 {
      i = i - 1
      if pred(self.at(i).or_panic_with_message("the index is within the slice")) {
        ret Option::Some(i)
      }
    }
    ret Option::None
  }

  // Whether every element is at or before the one after it, by `less`.
  // An empty slice and a one-element slice are sorted.
  pub fn is_sorted_by(self: &Self, less: (&T, &T) -> bool) -> bool {
    mut i := 1 as usize
    while i < slice_len(self) {
      if less(
        self.at(i).or_panic_with_message("the index is within the slice"),
        self.at(i - 1).or_panic_with_message("the index is within the slice"),
      ) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // The index `compare` answers zero for, or `None`. The slice must be
  // ordered the way `compare` orders it.
  pub fn binary_search_by(self: &Self, compare: (&T) -> i32) -> Option<usize> {
    mut lo := 0 as usize
    mut hi := slice_len(self)
    while lo < hi {
      mid := lo + (hi - lo) / 2
      c := compare(self.at(mid).or_panic_with_message("the index is within the slice"))
      if c == 0 {
        ret Option::Some(mid)
      }
      if c < 0 {
        lo = mid + 1
      } else {
        hi = mid
      }
    }
    ret Option::None
  }

  // The first element mutably, or `None` when empty.
  pub fn first_mut(mut self: &mut Self) -> Option<&mut T> {
    if slice_len(self) == 0 {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at_mut(self, 0) })
  }

  // The last element mutably, or `None` when empty.
  pub fn last_mut(mut self: &mut Self) -> Option<&mut T> {
    n := slice_len(self)
    if n == 0 {
      ret Option::None
    }
    ret Option::Some(unsafe { slice_at_mut(self, n - 1) })
  }

  // The views before and after `at`. The caller knows `at` is a
  // boundary it chose, so it must not exceed the length.
  pub fn split_at(self: &Self, at: usize) -> (&[T], &[T]) {
    if at >= slice_len(self) {
      ret (self, unsafe { slice_from_parts(slice_at(self, 0), 0) })
    }
    head := unsafe { slice_from_parts(slice_at(self, 0), at) }
    tail := unsafe { slice_from_parts(slice_at(self, at), slice_len(self) - at) }
    ret (head, tail)
  }

  // The first element and the rest, or `None` when empty.
  pub fn split_first(self: &Self) -> Option<(&T, &[T])> {
    if slice_len(self) == 0 {
      ret Option::None
    }
    ret Option::Some(
      (self.at(0).or_panic_with_message("the slice is not empty"), self.split_at(1).1)
    )
  }

  // The last element and the front, or `None` when empty.
  pub fn split_last(self: &Self) -> Option<(&T, &[T])> {
    n := slice_len(self)
    if n == 0 {
      ret Option::None
    }
    ret Option::Some(
      (self.at(n - 1).or_panic_with_message("the slice is not empty"), self.split_at(n - 1).0)
    )
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

// The comparisons walk elements, so they sit in a block that says
// what they need: `s.contains(&x)` on a `[Plain]` names `Plain` at the
// call rather than a line in here (ADR-0028).
impl<T: PartialEq>[T] {
  // Whether some element equals `value`, compared the way `==`
  // compares two elements.
  pub fn contains(self: &Self, value: &T) -> bool {
    mut i := 0 as usize
    while i < slice_len(self) {
      if *self.at(i).or_panic_with_message("the index is within the slice") == *value {
        ret true
      }
      i = i + 1
    }
    ret false
  }

  // Whether the view begins with `prefix`; an empty prefix matches.
  pub fn starts_with(self: &Self, prefix: &[T]) -> bool {
    n := slice_len(prefix)
    if n > slice_len(self) {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if *self.at(i).or_panic_with_message("the index is within the slice") != *prefix.at(i).or_panic_with_message(
        "the index is within the slice"
      ) {
        ret false
      }
      i = i + 1
    }
    ret true
  }

  // Whether the view ends with `suffix`; an empty suffix matches.
  pub fn ends_with(self: &Self, suffix: &[T]) -> bool {
    n := slice_len(suffix)
    m := slice_len(self)
    if n > m {
      ret false
    }
    mut i := 0 as usize
    while i < n {
      if *self.at(m - n + i).or_panic_with_message("the index is within the slice") != *suffix.at(i).or_panic_with_message(
        "the index is within the slice"
      ) {
        ret false
      }
      i = i + 1
    }
    ret true
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

impl<T> Iterator < &T > for SliceIter<T> {
  fn next(mut self: &mut Self) -> Option<&T> {
    if self.pos >= slice_len(self.s) {
      ret Option::None
    }
    v := unsafe { slice_at(self.s, self.pos) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}

impl<T, N: usize>[T; N] {
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
  pub fn into_iter(self: ArrayIntoIter < T, N >) -> ArrayIntoIter<T, N> {
    ret self
  }

  pub fn drop(mut self: ArrayIntoIter < T, N >) {
    mut i := self.pos
    while i < N {
      unsafe { drop_in_place(slice_at_mut(self.a.as_mut_slice(), i)) }
      i = i + 1
    }
  }
}

impl<T, N: usize> Iterator < T > for ArrayIntoIter<T, N> {
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

impl<T> Iterator < &mut T > for SliceIterMut<T> {
  fn next(mut self: &mut Self) -> Option<&mut T> {
    if self.pos >= slice_len(self.s) {
      ret Option::None
    }
    v := unsafe { slice_at_mut(self.s, self.pos) }
    self.pos = self.pos + 1
    ret Option::Some(v)
  }
}
