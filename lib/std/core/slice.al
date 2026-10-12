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

impl<T> [T] {
  // The element count.
  pub fn len(self: &Self) -> usize {
    ret slice_len(self)
  }

  pub fn is_empty(self: &Self) -> bool {
    ret slice_len(self) == 0
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
}
